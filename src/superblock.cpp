#include "edufs/superblock.hpp"
#include <algorithm>

namespace edufs {
namespace {
uint32_t ceil_div(uint64_t a, uint64_t b) { return static_cast<uint32_t>((a + b - 1) / b); }
constexpr uint32_t kGroupDescOffset = kSuperblockOffset + sizeof(Superblock);
}

bool valid_block_size(uint32_t bs) { return bs == 1024 || bs == 2048 || bs == 4096; }

Result<Superblock> make_superblock(const FormatOptions& opt) {
    const uint32_t bs = opt.block_size;
    if (!valid_block_size(bs) || opt.size_bytes < 64 * 1024) return Err::Invalid;
    const uint64_t blocks64 = opt.size_bytes / bs;
    if (blocks64 > 0xFFFFFFFFull) return Err::Invalid;
    const uint32_t total = static_cast<uint32_t>(blocks64);
    const uint32_t max_group_blocks = bs * 8u; // one bitmap block can describe this many blocks
    const uint32_t groups = ceil_div(total, max_group_blocks);
    const uint32_t desc_capacity = (ceil_div(kSuperblockOffset + sizeof(Superblock), bs) * bs - kGroupDescOffset) / sizeof(GroupDesc);
    if (groups == 0 || groups > desc_capacity) return Err::Unsupported;
    const uint32_t bpg = ceil_div(total, groups);
    const uint32_t per_blk = bs / kInodeSize;
    uint32_t wanted_inodes = opt.inodes ? opt.inodes : std::max<uint32_t>(32, total / 4);
    uint32_t ipg = ceil_div(ceil_div(wanted_inodes, groups), per_blk) * per_blk;
    if (ipg == 0 || ipg > bs * 8u || static_cast<uint64_t>(ipg) * groups > 0xFFFFFFFFull) return Err::Unsupported;
    const uint32_t inodes = ipg * groups;
    const uint32_t it_blocks = ipg / per_blk;

    Superblock sb{};
    sb.magic = kMagic; sb.version = kVersion; sb.block_size = bs;
    sb.total_blocks = total; sb.total_inodes = inodes; sb.inode_size = kInodeSize;
    sb.bb_start = ceil_div(kSuperblockOffset + sizeof(Superblock), bs); sb.bb_blocks = 1;
    sb.ib_start = sb.bb_start + 1; sb.ib_blocks = 1;
    sb.it_start = sb.ib_start + 1; sb.it_blocks = it_blocks;
    sb.data_start = sb.it_start + it_blocks;
    sb.root_inode = kRootIno; sb.state = kStateClean;
    sb.group_count = groups; sb.blocks_per_group = bpg; sb.inodes_per_group = ipg;
    sb.group_desc_offset = kGroupDescOffset; sb.group_desc_size = sizeof(GroupDesc);

    uint64_t free_blocks = 0;
    for (uint32_t g = 0; g < groups; ++g) {
        const uint32_t first = g * bpg;
        const uint32_t count = std::min<uint32_t>(bpg, total - first);
        const uint32_t data_first = (g == 0) ? sb.data_start : first + 2 + it_blocks;
        if (data_first >= first + count) return Err::NoSpace;
        free_blocks += static_cast<uint64_t>(first + count - data_first);
    }
    if (free_blocks < 4) return Err::NoSpace;
    sb.free_blocks = static_cast<uint32_t>(free_blocks);
    sb.free_inodes = inodes;
    return sb;
}

Err validate_superblock(const Superblock& sb, uint64_t image_bytes) {
    if (sb.magic != kMagic) return Err::BadMagic;
    if (sb.version != kVersion) return Err::Corrupt;
    if (!valid_block_size(sb.block_size) || sb.inode_size != kInodeSize) return Err::Corrupt;
    const uint32_t bs = sb.block_size, per_blk = bs / kInodeSize;
    const uint32_t max_group_blocks = bs * 8u;
    const uint32_t expected_groups = sb.total_blocks ? ceil_div(sb.total_blocks, max_group_blocks) : 0;
    const uint32_t expected_bpg = expected_groups ? ceil_div(sb.total_blocks, expected_groups) : 0;
    const uint32_t desc_capacity = (ceil_div(kSuperblockOffset + sizeof(Superblock), bs) * bs - kGroupDescOffset) / sizeof(GroupDesc);
    const bool ok =
        sb.total_blocks > 0 && static_cast<uint64_t>(sb.total_blocks) * bs <= image_bytes &&
        sb.total_inodes >= kFirstFreeIno && sb.root_inode == kRootIno &&
        sb.group_count == expected_groups && sb.group_count > 0 && sb.group_count <= desc_capacity &&
        sb.blocks_per_group == expected_bpg && sb.inodes_per_group > 0 &&
        static_cast<uint64_t>(sb.inodes_per_group) * sb.group_count == sb.total_inodes &&
        sb.group_desc_offset == kGroupDescOffset && sb.group_desc_size == sizeof(GroupDesc) &&
        sb.bb_start == ceil_div(kSuperblockOffset + sizeof(Superblock), bs) && sb.bb_blocks == 1 &&
        sb.ib_start == sb.bb_start + 1 && sb.ib_blocks == 1 &&
        sb.it_start == sb.ib_start + 1 && sb.it_blocks == sb.inodes_per_group / per_blk &&
        sb.data_start == sb.it_start + sb.it_blocks && sb.data_start < sb.total_blocks &&
        sb.free_blocks <= sb.total_blocks && sb.free_inodes <= sb.total_inodes;
    return ok ? Err::Ok : Err::Corrupt;
}
} // namespace edufs
