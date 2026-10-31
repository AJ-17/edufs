#pragma once
#include <cstddef>
#include <cstdint>

namespace edufs {
constexpr uint16_t kModeRegular = 0100000;
constexpr uint16_t kModeDirectory = 0040000;
constexpr uint16_t kModeMask = 0170000;
constexpr uint8_t kDirRegular = 1;
constexpr uint8_t kDirDirectory = 2;
constexpr size_t kNameMax = 56;

#pragma pack(push, 1)
struct Inode {
    uint16_t mode = 0;
    uint16_t links = 0;
    uint32_t size = 0;
    uint32_t atime = 0;
    uint32_t mtime = 0;
    uint32_t ctime = 0;
    uint32_t blocks = 0; // number of data blocks, not 512-byte sectors
    uint32_t direct[12]{};
    uint32_t indirect = 0;
    uint32_t double_indirect = 0;
    uint8_t reserved[48]{};
};
struct DirEntry {
    uint32_t inode = 0;
    uint16_t rec_len = 64;
    uint8_t name_len = 0;
    uint8_t type = 0;
    char name[kNameMax]{};
};
#pragma pack(pop)
static_assert(sizeof(Inode) == 128);
static_assert(sizeof(DirEntry) == 64);
} // namespace edufs
