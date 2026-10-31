#include "edufs/inspect.hpp"
#include <algorithm>
#include <ctime>
#include <sstream>
#include <vector>

namespace edufs {
namespace {
Result<std::vector<GroupDesc>> inspect_groups(Fs& fs) {
    const auto& s = fs.super();
    std::vector<GroupDesc> groups(s.group_count);
    if (fs.device().read_bytes(s.group_desc_offset, groups.data(), groups.size() * sizeof(GroupDesc)) != Err::Ok) return Err::Io;
    return groups;
}
Result<uint64_t> inode_offset(Fs& fs, uint32_t ino) {
    const auto& s = fs.super();
    if (ino == 0 || ino > s.total_inodes) return Err::Invalid;
    auto groups = inspect_groups(fs); if (!groups) return groups.err;
    const uint32_t g = (ino - 1) / s.inodes_per_group;
    const uint32_t local = (ino - 1) % s.inodes_per_group;
    return static_cast<uint64_t>(groups.value[g].inode_table) * s.block_size + static_cast<uint64_t>(local) * sizeof(Inode);
}
}

std::string format_time(uint32_t t) {
    std::time_t tt = t; char buf[32]; std::tm tmv{};
    localtime_r(&tt, &tmv); std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

std::string dump_super(Fs& fs) {
    const Superblock& s = fs.super();
    auto range = [](uint32_t start, uint32_t n) {
        return n == 0 ? std::string("(empty)") : "blocks " + std::to_string(start) + ".." + std::to_string(start + n - 1);
    };
    std::ostringstream o;
    o << "Superblock (at byte " << kSuperblockOffset << ")\n"
      << "  magic:            0x" << std::hex << s.magic << std::dec << (s.magic == kMagic ? " (EDFS)" : " (BAD)") << "\n"
      << "  version:          " << s.version << "\n"
      << "  block size:       " << s.block_size << "\n"
      << "  total blocks:     " << s.total_blocks << "\n"
      << "  free blocks:      " << s.free_blocks << "\n"
      << "  total inodes:     " << s.total_inodes << "\n"
      << "  free inodes:      " << s.free_inodes << "\n"
      << "  inode size:       " << s.inode_size << "\n"
      << "  block bitmap:     " << range(s.bb_start, s.bb_blocks) << " (per group)\n"
      << "  inode bitmap:     " << range(s.ib_start, s.ib_blocks) << " (per group)\n"
      << "  inode table:      " << range(s.it_start, s.it_blocks) << " (group 0)\n"
      << "  first data block: " << s.data_start << "\n"
      << "  root inode:       " << s.root_inode << "\n"
      << "  state:            " << (s.state == kStateClean ? "clean" : "mounted/dirty") << "\n"
      << "  mount count:      " << s.mount_count << "\n"
      << "  last mount:       " << format_time(s.mtime) << "\n"
      << "  last write:       " << format_time(s.wtime) << "\n"
      << "  block groups:     " << s.group_count << "\n"
      << "  blocks/group:     " << s.blocks_per_group << "\n"
      << "  inodes/group:     " << s.inodes_per_group << "\n";
    auto groups = inspect_groups(fs);
    if (groups) for (size_t i = 0; i < groups.value.size(); ++i) {
        const auto& g = groups.value[i];
        o << "  group " << i << ": blocks " << g.first_block << ".." << (g.first_block + g.blocks_count - 1)
          << ", block bitmap " << g.block_bitmap << ", inode bitmap " << g.inode_bitmap
          << ", inode table " << g.inode_table << ", free blocks " << g.free_blocks
          << ", free inodes " << g.free_inodes << "\n";
    }
    return o.str();
}

Result<std::string> dump_inode(Fs& fs, uint32_t ino) {
    auto off = inode_offset(fs, ino); if (!off) return off.err;
    Inode in{};
    if (fs.device().read_bytes(off.value, &in, sizeof in) != Err::Ok) return Err::Io;
    if (in.mode == 0) return Err::NotFound;
    std::ostringstream o;
    o << "Inode " << ino << "\n  type: " << (((in.mode & kModeMask) == kModeDirectory) ? "directory" : "regular")
      << "\n  mode: 0" << std::oct << in.mode << std::dec << "\n  links: " << in.links
      << "\n  size: " << in.size << " bytes\n  data blocks: " << in.blocks << "\n  direct: ";
    for (uint32_t i = 0; i < 12; ++i) { if (i) o << ','; o << in.direct[i]; }
    o << "\n  indirect: " << in.indirect << "\n  double indirect: " << in.double_indirect
      << "\n  mtime: " << format_time(in.mtime) << "\n";
    return o.str();
}

std::string dump_bitmap(Fs& fs, bool inode_bitmap, uint32_t first, uint32_t count) {
    const auto& s = fs.super();
    const uint32_t bits = inode_bitmap ? s.total_inodes : s.total_blocks;
    const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(bits, static_cast<uint64_t>(first) + count));
    auto groups_result = inspect_groups(fs); if (!groups_result) return "group descriptor read failed\n";
    const auto& groups = groups_result.value;
    std::vector<std::vector<uint8_t>> raw(groups.size(), std::vector<uint8_t>(s.block_size));
    for (size_t g = 0; g < groups.size(); ++g) {
        uint32_t block = inode_bitmap ? groups[g].inode_bitmap : groups[g].block_bitmap;
        if (fs.device().read_block(block, raw[g]) != Err::Ok) return "bitmap read failed\n";
    }
    std::ostringstream o;
    o << (inode_bitmap ? "Inode" : "Block") << " bitmap bits " << first << ".." << (end ? end - 1 : 0) << " (1=used, 0=free)\n";
    for (uint32_t i = first; i < end; ++i) {
        const uint32_t g = inode_bitmap ? i / s.inodes_per_group : i / s.blocks_per_group;
        const uint32_t local = inode_bitmap ? i % s.inodes_per_group : i % s.blocks_per_group;
        const bool used = (raw[g][local / 8] >> (local % 8)) & 1u;
        o << i << ':' << (used ? '1' : '0') << ((i - first + 1) % 16 == 0 ? '\n' : ' ');
    }
    if (end > first && (end - first) % 16) o << '\n';
    return o.str();
}

Result<std::string> dump_directory(Fs& fs, const std::string& path) {
    auto entries = fs.list(path); if (!entries) return entries.err;
    std::ostringstream o; o << "Directory " << path << "\n";
    for (const auto& e : entries.value) o << "inode=" << e.inode << " type=" << (e.type == kDirDirectory ? "dir" : "file") << " name=" << e.name << '\n';
    return o.str();
}

Result<std::string> dump_blockmap(Fs& fs, const std::string& path) {
    auto st = fs.stat(path); if (!st) return st.err;
    auto off = inode_offset(fs, st.value.inode); if (!off) return off.err;
    Inode in{};
    if (fs.device().read_bytes(off.value, &in, sizeof in) != Err::Ok) return Err::Io;
    std::ostringstream o; o << "Block map for " << path << " (inode " << st.value.inode << ")\n";
    uint32_t logical = 0;
    for (uint32_t b : in.direct) { if (!b) break; o << logical++ << " -> " << b << "\n"; }
    const uint32_t per = fs.super().block_size / sizeof(uint32_t);
    if (in.indirect) {
        std::vector<uint32_t> ptr(per); if (fs.device().read_block(in.indirect, reinterpret_cast<uint8_t*>(ptr.data())) != Err::Ok) return Err::Io;
        for (uint32_t b : ptr) { if (!b) break; o << logical++ << " -> " << b << " (single indirect)\n"; }
    }
    if (in.double_indirect) {
        std::vector<uint32_t> outer(per); if (fs.device().read_block(in.double_indirect, reinterpret_cast<uint8_t*>(outer.data())) != Err::Ok) return Err::Io;
        for (uint32_t mid : outer) { if (!mid) continue; std::vector<uint32_t> inner(per); if (fs.device().read_block(mid, reinterpret_cast<uint8_t*>(inner.data())) != Err::Ok) return Err::Io; for (uint32_t b : inner) { if (!b) break; o << logical++ << " -> " << b << " (double indirect)\n"; } }
    }
    return o.str();
}
} // namespace edufs
