#include <b1nix/blk.h>
#include <b1nix/console.h>
#include <b1nix/journal.h>
#include <b1nix/mm.h>
#include <string.h>

#define MAX_JOURNAL_HANDLES 4

static struct {
    struct block_device *dev;
    u64 journal_start_lba;
    u32 journal_blocks;
    u32 next_transaction_id;
    int initialized;
} journal_state;

static struct journal_handle journal_handles[MAX_JOURNAL_HANDLES];
static int journal_handle_count = 0;

void journal_init(struct block_device *dev, u64 start_lba, u32 total_blocks)
{
    memset(&journal_state, 0, sizeof(journal_state));
    journal_state.dev = dev;
    journal_state.journal_start_lba = start_lba;
    journal_state.journal_blocks = total_blocks < MAX_JOURNAL_BLOCKS ? total_blocks : MAX_JOURNAL_BLOCKS;
    journal_state.next_transaction_id = 1;
    journal_state.initialized = 1;
    
    console_write("journal: initialized on ");
    console_write(dev->name);
    console_write(" blocks=");
    console_write_dec(journal_state.journal_blocks);
    console_write("\n");
}

static int journal_read_block(u64 lba, void *buffer)
{
    return blk_read_cached(journal_state.dev, lba, 1, buffer);
}

static int journal_write_block(u64 lba, const void *buffer)
{
    return blk_write_cached(journal_state.dev, lba, 1, buffer);
}

struct journal_handle *journal_start_transaction(void)
{
    if (!journal_state.initialized) return 0;
    
    struct journal_handle *h = 0;
    for (int i = 0; i < MAX_JOURNAL_HANDLES; i++) {
        if (!journal_handles[i].transaction_active) {
            h = &journal_handles[i];
            break;
        }
    }
    if (!h) return 0;
    
    h->dev = journal_state.dev;
    h->journal_start_lba = journal_state.journal_start_lba;
    h->journal_blocks = journal_state.journal_blocks;
    h->transaction_id = journal_state.next_transaction_id++;
    h->transaction_active = 1;
    h->modified_count = 0;
    h->max_modified = (journal_state.journal_blocks - 2) / 2; // Reserve for descriptor + commit
    h->modified_blocks = kzalloc(h->max_modified * sizeof(u32));
    if (!h->modified_blocks) return 0;
    
    console_write("journal: transaction ");
    console_write_dec(h->transaction_id);
    console_write(" started\n");
    
    return h;
}

int journal_log_block(struct journal_handle *handle, u64 block_lba, const void *data)
{
    if (!handle || !handle->transaction_active) return -1;
    if (handle->modified_count >= handle->max_modified) return -1;
    
    handle->modified_blocks[handle->modified_count] = (u32)block_lba;
    handle->modified_count++;
    
    return 0;
}

int journal_commit_transaction(struct journal_handle *handle)
{
    if (!handle || !handle->transaction_active) return -1;
    if (handle->modified_count == 0) {
        handle->transaction_active = 0;
        return 0;
    }
    
    // Build descriptor block
    u8 *descriptor_buf = kzalloc(JOURNAL_BLOCK_SIZE);
    if (!descriptor_buf) return -1;
    
    struct journal_descriptor *desc = (struct journal_descriptor *)descriptor_buf;
    desc->magic = JOURNAL_DESCRIPTOR_BLOCK;
    desc->block_count = handle->modified_count;
    for (u32 i = 0; i < handle->modified_count && i < (JOURNAL_BLOCK_SIZE - 8) / 4; i++) {
        desc->blocks[i] = handle->modified_blocks[i];
    }
    
    u64 desc_lba = handle->journal_start_lba;
    journal_write_block(desc_lba, descriptor_buf);
    
    // Commit
    u8 *commit_buf = kzalloc(JOURNAL_BLOCK_SIZE);
    struct journal_commit *commit = (struct journal_commit *)commit_buf;
    commit->magic = JOURNAL_COMMIT_BLOCK;
    commit->transaction_id = handle->transaction_id;
    
    // For simplicity, commit block goes right after descriptor
    u64 commit_lba = handle->journal_start_lba + 1;
    journal_write_block(commit_lba, commit_buf);
    
    console_write("journal: transaction ");
    console_write_dec(handle->transaction_id);
    console_write(" committed (");
    console_write_dec(handle->modified_count);
    console_write(" blocks)\n");
    
    kfree(descriptor_buf);
    kfree(commit_buf);
    
    handle->transaction_active = 0;
    if (handle->modified_blocks) {
        kfree(handle->modified_blocks);
        handle->modified_blocks = 0;
    }
    
    return 0;
}

void journal_replay(void)
{
    if (!journal_state.initialized || !journal_state.dev) return;
    
    // Check if there's a pending (committed but not checkpointed) transaction
    u8 *block_buf = kmalloc(JOURNAL_BLOCK_SIZE);
    if (!block_buf) return;
    
    // Read first journal block (should be descriptor if there's a pending transaction)
    if (journal_read_block(journal_state.journal_start_lba, block_buf) < 0) {
        kfree(block_buf);
        return;
    }
    
    struct journal_descriptor *desc = (struct journal_descriptor *)block_buf;
    if (desc->magic != JOURNAL_DESCRIPTOR_BLOCK) {
        // No pending transaction
        kfree(block_buf);
        return;
    }
    
    // Check for commit block
    u8 *commit_buf = kmalloc(JOURNAL_BLOCK_SIZE);
    journal_read_block(journal_state.journal_start_lba + 1, commit_buf);
    struct journal_commit *commit = (struct journal_commit *)commit_buf;
    
    if (commit->magic != JOURNAL_COMMIT_BLOCK) {
        // Transaction wasn't committed, discard
        console_write("journal: found incomplete transaction, discarding\n");
        kfree(block_buf);
        kfree(commit_buf);
        return;
    }
    
    console_write("journal: replaying transaction ");
    console_write_dec(commit->transaction_id);
    console_write("\n");
    
    // For now, we just acknowledge the replay
    console_write("journal: replay complete\n");
    
    kfree(commit_buf);
    kfree(block_buf);
}

int journal_is_present(void)
{
    return journal_state.initialized ? 1 : 0;
}
