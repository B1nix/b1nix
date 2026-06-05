#ifndef B1NIX_LOOP_H
#define B1NIX_LOOP_H

#include <b1nix/blk.h>

struct block_device *loop_register_file(const char *path, const char *name);

#endif
