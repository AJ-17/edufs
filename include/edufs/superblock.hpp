#pragma once
// Pure functions about the superblock: no disk access, so they are easy to test.
#include "edufs/errors.hpp"
#include "edufs/layout.hpp"

namespace edufs {

struct FormatOptions {
    uint64_t size_bytes = 8ull << 20;
    uint32_t block_size = 1024;  // 1024, 2048 or 4096
    uint32_t inodes = 0;         // 0 = automatic (one inode per 4 blocks, at least 32)
};

bool valid_block_size(uint32_t bs);

// Work out the whole disk layout for the given options.
//   Err::Invalid  - unsupported block size or image too small to be useful
//   Err::NoSpace  - the metadata alone would not fit in the image
Result<Superblock> make_superblock(const FormatOptions& opt);

// Check that a superblock read from disk is believable.
//   Err::BadMagic - not an EduFS image
//   Err::Corrupt  - magic is right but the numbers do not add up
Err validate_superblock(const Superblock& sb, uint64_t image_bytes);

}  // namespace edufs