#pragma once
// On-disk structures. Every struct here is written to the image byte-for-byte,
// so each one is packed and size-checked at compile time.
//
// Image layout (block size bs):
//
//   bytes 0..1023       boot area (unused)
//   bytes 1024..1279    primary superblock (256 bytes)
//   bytes 1280..        compact group descriptor table, before bitmap blocks
//   group 0             primary block bitmap, inode bitmap, inode table, data
//   later groups        group-local block bitmap, inode bitmap, inode table, data
//
// Block 0 is always reserved, so a block pointer of 0 will mean "no block".
// Inode numbers start at 1 (inode 1 reserved, root directory will be inode 2).
#include <bit>
#include <cstddef>
#include <cstdint>

namespace edufs {

static_assert(std::endian::native == std::endian::little,
              "EduFS images are little-endian (like ext2); big-endian hosts are unsupported");

constexpr uint32_t kMagic = 0x53464445;  // stored little-endian: bytes 'E' 'D' 'F' 'S'
constexpr uint32_t kVersion = 1;
constexpr uint32_t kSuperblockOffset = 1024;
constexpr uint32_t kInodeSize = 128;
constexpr uint32_t kRootIno = 2;
constexpr uint32_t kFirstFreeIno = 3;

constexpr uint32_t kStateDirty = 0;  // mounted, or never cleanly unmounted
constexpr uint32_t kStateClean = 1;

#pragma pack(push, 1)
struct Superblock {
    uint32_t magic;
    uint32_t version;
    uint32_t block_size;
    uint32_t total_blocks;
    uint32_t total_inodes;
    uint32_t free_blocks;
    uint32_t free_inodes;
    uint32_t inode_size;
    uint32_t bb_start, bb_blocks;   // block bitmap
    uint32_t ib_start, ib_blocks;   // inode bitmap
    uint32_t it_start, it_blocks;   // inode table
    uint32_t data_start;            // first data block
    uint32_t root_inode;
    uint32_t state;                 // kStateClean / kStateDirty
    uint32_t mount_count;
    uint32_t mtime;                 // last mount
    uint32_t wtime;                 // last superblock write
    uint32_t group_count;
    uint32_t blocks_per_group;
    uint32_t inodes_per_group;
    uint32_t group_desc_offset;
    uint32_t group_desc_size;
    uint32_t reserved[39];          // room to grow without changing the size
};

// One descriptor per block group. The first inode number in a group is derived
// from group index * inodes_per_group + 1.
struct GroupDesc {
    uint32_t block_bitmap;
    uint32_t inode_bitmap;
    uint32_t inode_table;
    uint32_t first_block;
    uint32_t blocks_count;
    uint32_t inodes_count;
    uint32_t free_blocks;
    uint32_t free_inodes;
};
#pragma pack(pop)

// Compile-time guards: if anyone changes the struct, the build breaks instead of the disk format.
static_assert(sizeof(Superblock) == 256, "Superblock must be exactly 256 bytes");
static_assert(offsetof(Superblock, magic) == 0, "magic must be the first field");
static_assert(offsetof(Superblock, block_size) == 8, "block_size offset changed");
static_assert(offsetof(Superblock, data_start) == 56, "data_start offset changed");
static_assert(sizeof(GroupDesc) == 32, "GroupDesc must be exactly 32 bytes");

}  // namespace edufs