#include "edufs/fs.hpp"

#include <algorithm>
#include <ctime>
#include <cstring>
#include <limits>
#include <sstream>

namespace edufs {
namespace {
uint32_t now_s() { return static_cast<uint32_t>(std::time(nullptr)); }
std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> out;
    std::string part;
    for (char c : path) {
        if (c == '/') { if (!part.empty()) { out.push_back(part); part.clear(); } }
        else part.push_back(c);
    }
    if (!part.empty()) out.push_back(part);
    return out;
}
}

Fs::~Fs() { if (mounted_) unmount(); }

Err Fs::write_super() {
    if (groups_.size() == sb_.group_count && sb_.group_count != 0) {
        sb_.free_blocks = static_cast<uint32_t>(block_bitmap_.count_free());
        sb_.free_inodes = static_cast<uint32_t>(inode_bitmap_.count_free());
        if (Err e = save_groups(); e != Err::Ok) return e;
    }
    sb_.wtime = now_s();
    return dev_.write_bytes(kSuperblockOffset, &sb_, sizeof sb_);
}

Err Fs::format(const std::string& path, const FormatOptions& opt) {
    auto made = make_superblock(opt);
    if (!made) return made.err;
    Superblock sb = made.value;
    sb.mtime = sb.wtime = now_s();
    const uint64_t image_size = static_cast<uint64_t>(sb.total_blocks) * sb.block_size;
    if (Err e = BlockDevice::create(path, image_size); e != Err::Ok) return e;
    BlockDevice dev;
    if (Err e = dev.open(path, sb.block_size); e != Err::Ok) return e;
    if (Err e = dev.write_bytes(kSuperblockOffset, &sb, sizeof sb); e != Err::Ok) return e;

    std::vector<GroupDesc> groups(sb.group_count);
    for (uint32_t g = 0; g < sb.group_count; ++g) {
        const uint32_t first = g * sb.blocks_per_group;
        const uint32_t count = std::min<uint32_t>(sb.blocks_per_group, sb.total_blocks - first);
        GroupDesc& gd = groups[g];
        gd.first_block = first; gd.blocks_count = count; gd.inodes_count = sb.inodes_per_group;
        gd.block_bitmap = g == 0 ? sb.bb_start : first;
        gd.inode_bitmap = g == 0 ? sb.ib_start : first + 1;
        gd.inode_table = g == 0 ? sb.it_start : first + 2;
    }
    Bitmap bb(sb.total_blocks);
    for (const auto& gd : groups) {
        const uint32_t data_first = gd.first_block == 0 ? sb.data_start : gd.inode_table + sb.it_blocks;
        if (data_first >= gd.first_block + gd.blocks_count) return Err::NoSpace;
        for (uint32_t b = gd.first_block; b < data_first; ++b) bb.set(b);
    }
    bb.set(sb.data_start); // root directory data block
    Bitmap ib(sb.total_inodes);
    ib.set(0); ib.set(1); // inode 1 reserved, inode 2 is root
    sb.free_blocks = static_cast<uint32_t>(bb.count_free());
    sb.free_inodes = static_cast<uint32_t>(ib.count_free());
    for (uint32_t g = 0; g < sb.group_count; ++g) {
        auto& gd = groups[g];
        uint32_t free_b = 0, free_i = 0;
        for (uint32_t b = gd.first_block; b < gd.first_block + gd.blocks_count; ++b) if (!bb.test(b)) ++free_b;
        const uint32_t first_ino_idx = g * sb.inodes_per_group;
        for (uint32_t i = first_ino_idx; i < first_ino_idx + gd.inodes_count; ++i) if (!ib.test(i)) ++free_i;
        gd.free_blocks = free_b; gd.free_inodes = free_i;
        std::vector<uint8_t> raw(sb.block_size, 0);
        for (uint32_t local = 0; local < gd.blocks_count; ++local) if (bb.test(gd.first_block + local)) raw[local / 8] |= static_cast<uint8_t>(1u << (local % 8));
        if (Err e = dev.write_block(gd.block_bitmap, raw); e != Err::Ok) return e;
        std::fill(raw.begin(), raw.end(), 0);
        for (uint32_t local = 0; local < gd.inodes_count; ++local) if (ib.test(first_ino_idx + local)) raw[local / 8] |= static_cast<uint8_t>(1u << (local % 8));
        if (Err e = dev.write_block(gd.inode_bitmap, raw); e != Err::Ok) return e;
    }
    if (Err e = dev.write_bytes(sb.group_desc_offset, groups.data(), groups.size() * sizeof(GroupDesc)); e != Err::Ok) return e;

    Inode root{};
    root.mode = kModeDirectory | 0755; root.links = 2; root.size = sb.block_size;
    root.atime = root.mtime = root.ctime = now_s(); root.blocks = 1; root.direct[0] = sb.data_start;
    const uint64_t root_offset = static_cast<uint64_t>(groups[0].inode_table) * sb.block_size + (kRootIno - 1) * sizeof(Inode);
    if (Err e = dev.write_bytes(root_offset, &root, sizeof root); e != Err::Ok) return e;
    std::vector<uint8_t> dir(sb.block_size, 0);
    DirEntry dot{}; dot.inode = kRootIno; dot.name_len = 1; dot.type = kDirDirectory; std::memcpy(dot.name, ".", 1);
    DirEntry dotdot{}; dotdot.inode = kRootIno; dotdot.name_len = 2; dotdot.type = kDirDirectory; std::memcpy(dotdot.name, "..", 2);
    std::memcpy(dir.data(), &dot, sizeof dot); std::memcpy(dir.data() + sizeof dot, &dotdot, sizeof dotdot);
    if (Err e = dev.write_block(sb.data_start, dir); e != Err::Ok) return e;
    if (Err e = dev.write_bytes(kSuperblockOffset, &sb, sizeof sb); e != Err::Ok) return e;
    return dev.flush();
}

Err Fs::load_groups() {
    const uint32_t desc_capacity = (static_cast<uint32_t>((kSuperblockOffset + sizeof(Superblock) + sb_.block_size - 1) / sb_.block_size) * sb_.block_size - sb_.group_desc_offset) / sizeof(GroupDesc);
    if (sb_.group_count == 0 || sb_.group_count > desc_capacity) return Err::Corrupt;
    groups_.resize(sb_.group_count);
    if (dev_.read_bytes(sb_.group_desc_offset, groups_.data(), groups_.size() * sizeof(GroupDesc)) != Err::Ok) return Err::Io;
    for (uint32_t g = 0; g < sb_.group_count; ++g) {
        const auto& gd = groups_[g];
        const uint32_t first = g * sb_.blocks_per_group;
        const uint32_t count = std::min<uint32_t>(sb_.blocks_per_group, sb_.total_blocks - first);
        const uint32_t expected_bb = g == 0 ? sb_.bb_start : first;
        const uint32_t expected_ib = g == 0 ? sb_.ib_start : first + 1;
        const uint32_t expected_it = g == 0 ? sb_.it_start : first + 2;
        if (gd.first_block != first || gd.blocks_count != count || gd.inodes_count != sb_.inodes_per_group ||
            gd.block_bitmap != expected_bb || gd.inode_bitmap != expected_ib || gd.inode_table != expected_it ||
            static_cast<uint64_t>(gd.inode_table + sb_.it_blocks) > static_cast<uint64_t>(first) + count) return Err::Corrupt;
    }
    return Err::Ok;
}

Err Fs::save_groups() {
    if (groups_.size() != sb_.group_count) return Err::Corrupt;
    for (uint32_t g = 0; g < sb_.group_count; ++g) {
        auto& gd = groups_[g]; gd.free_blocks = 0; gd.free_inodes = 0;
        for (uint32_t b = gd.first_block; b < gd.first_block + gd.blocks_count; ++b) if (!block_bitmap_.test(b)) ++gd.free_blocks;
        const uint32_t first = g * sb_.inodes_per_group;
        for (uint32_t i = first; i < first + gd.inodes_count; ++i) if (!inode_bitmap_.test(i)) ++gd.free_inodes;
    }
    return dev_.write_bytes(sb_.group_desc_offset, groups_.data(), groups_.size() * sizeof(GroupDesc));
}

Err Fs::load_bitmaps() {
    block_bitmap_ = Bitmap(sb_.total_blocks);
    inode_bitmap_ = Bitmap(sb_.total_inodes);
    std::vector<uint8_t> raw(sb_.block_size);
    for (uint32_t g = 0; g < groups_.size(); ++g) {
        const auto& gd = groups_[g];
        if (dev_.read_block(gd.block_bitmap, raw) != Err::Ok) return Err::Io;
        for (uint32_t local = 0; local < gd.blocks_count; ++local) if ((raw[local / 8] >> (local % 8)) & 1u) block_bitmap_.set(gd.first_block + local);
        if (dev_.read_block(gd.inode_bitmap, raw) != Err::Ok) return Err::Io;
        const uint32_t first_ino_idx = g * sb_.inodes_per_group;
        for (uint32_t local = 0; local < gd.inodes_count; ++local) if ((raw[local / 8] >> (local % 8)) & 1u) inode_bitmap_.set(first_ino_idx + local);
    }
    for (const auto& gd : groups_) {
        const uint32_t data_first = gd.first_block == 0 ? sb_.data_start : gd.inode_table + sb_.it_blocks;
        for (uint32_t b = gd.first_block; b < data_first; ++b) if (!block_bitmap_.test(b)) return Err::Corrupt;
    }
    if (!block_bitmap_.test(sb_.data_start) || !inode_bitmap_.test(kRootIno - 1)) return Err::Corrupt;
    return Err::Ok;
}
Err Fs::save_block_bitmap() {
    std::vector<uint8_t> raw(sb_.block_size, 0);
    for (const auto& gd : groups_) {
        std::fill(raw.begin(), raw.end(), 0);
        for (uint32_t local = 0; local < gd.blocks_count; ++local) if (block_bitmap_.test(gd.first_block + local)) raw[local / 8] |= static_cast<uint8_t>(1u << (local % 8));
        if (dev_.write_block(gd.block_bitmap, raw) != Err::Ok) return Err::Io;
    }
    return Err::Ok;
}
Err Fs::save_inode_bitmap() {
    std::vector<uint8_t> raw(sb_.block_size, 0);
    for (uint32_t g = 0; g < groups_.size(); ++g) {
        const auto& gd = groups_[g]; std::fill(raw.begin(), raw.end(), 0);
        const uint32_t first = g * sb_.inodes_per_group;
        for (uint32_t local = 0; local < gd.inodes_count; ++local) if (inode_bitmap_.test(first + local)) raw[local / 8] |= static_cast<uint8_t>(1u << (local % 8));
        if (dev_.write_block(gd.inode_bitmap, raw) != Err::Ok) return Err::Io;
    }
    return Err::Ok;
}

Err Fs::mount(const std::string& path) {
    if (mounted_) return Err::Invalid;
    if (Err e = dev_.open(path, 1024); e != Err::Ok) return e;
    Superblock sb{};
    if (dev_.read_bytes(kSuperblockOffset, &sb, sizeof sb) != Err::Ok) { dev_.close(); return Err::BadMagic; }
    if (Err e = validate_superblock(sb, dev_.size_bytes()); e != Err::Ok) { dev_.close(); return e; }
    dev_.set_block_size(sb.block_size);
    sb_ = sb;
    if (Err e = load_groups(); e != Err::Ok) { dev_.close(); return e; }
    if (Err e = load_bitmaps(); e != Err::Ok) { dev_.close(); return e; }
    was_clean_ = sb_.state == kStateClean;
    sb_.state = kStateDirty;
    sb_.mount_count++;
    sb_.mtime = now_s();
    if (Err e = write_super(); e != Err::Ok) { dev_.close(); return e; }
    mounted_ = true;
    return Err::Ok;
}

Err Fs::unmount() {
    if (!mounted_) return Err::Ok;
    sb_.state = kStateClean;
    Err e = write_super();
    if (dev_.flush() != Err::Ok && e == Err::Ok) e = Err::Io;
    dev_.close();
    mounted_ = false;
    return e;
}

Result<uint32_t> Fs::alloc_block() {
    auto n = block_bitmap_.find_first_zero(sb_.data_start);
    if (!n) return Err::NoSpace;
    if (!block_bitmap_.set(*n)) return Err::Corrupt;
    if (save_block_bitmap() != Err::Ok) return Err::Io;
    if (sb_.free_blocks) --sb_.free_blocks;
    if (write_super() != Err::Ok) return Err::Io;
    std::vector<uint8_t> zero(sb_.block_size, 0);
    if (dev_.write_block(static_cast<uint32_t>(*n), zero) != Err::Ok) return Err::Io;
    return static_cast<uint32_t>(*n);
}
bool Fs::is_data_block(uint32_t block) const {
    if (block >= sb_.total_blocks) return false;
    const uint32_t g = block / sb_.blocks_per_group;
    if (g >= groups_.size()) return false;
    const auto& gd = groups_[g];
    const uint32_t first_data = gd.first_block == 0 ? sb_.data_start : gd.inode_table + sb_.it_blocks;
    return block >= first_data && block < gd.first_block + gd.blocks_count;
}
Err Fs::free_block(uint32_t block) {
    if (!is_data_block(block) || !block_bitmap_.free(block)) return Err::Corrupt;
    if (save_block_bitmap() != Err::Ok) return Err::Io;
    ++sb_.free_blocks;
    return write_super();
}
Result<uint32_t> Fs::alloc_inode() {
    auto n = inode_bitmap_.find_first_zero(2);
    if (!n) return Err::NoInodes;
    inode_bitmap_.set(*n);
    if (save_inode_bitmap() != Err::Ok) return Err::Io;
    if (sb_.free_inodes) --sb_.free_inodes;
    if (write_super() != Err::Ok) return Err::Io;
    return static_cast<uint32_t>(*n + 1);
}
Err Fs::free_inode(uint32_t ino) {
    if (ino < kFirstFreeIno || ino > sb_.total_inodes || !inode_bitmap_.free(ino - 1)) return Err::Corrupt;
    if (save_inode_bitmap() != Err::Ok) return Err::Io;
    ++sb_.free_inodes;
    Inode empty{};
    return write_inode(ino, empty) == Err::Ok ? write_super() : Err::Io;
}
Err Fs::read_inode(uint32_t ino, Inode& out) {
    if (ino == 0 || ino > sb_.total_inodes || !inode_bitmap_.test(ino - 1)) return Err::NotFound;
    const uint32_t group = (ino - 1) / sb_.inodes_per_group;
    const uint32_t local = (ino - 1) % sb_.inodes_per_group;
    uint64_t off = static_cast<uint64_t>(groups_[group].inode_table) * sb_.block_size + static_cast<uint64_t>(local) * sizeof(Inode);
    return dev_.read_bytes(off, &out, sizeof out);
}
Err Fs::write_inode(uint32_t ino, const Inode& in) {
    if (ino == 0 || ino > sb_.total_inodes) return Err::Invalid;
    const uint32_t group = (ino - 1) / sb_.inodes_per_group;
    const uint32_t local = (ino - 1) % sb_.inodes_per_group;
    uint64_t off = static_cast<uint64_t>(groups_[group].inode_table) * sb_.block_size + static_cast<uint64_t>(local) * sizeof(Inode);
    return dev_.write_bytes(off, &in, sizeof in);
}

Result<uint32_t> Fs::get_data_block(Inode& ino, uint32_t logical, bool allocate) {
    const uint32_t per = sb_.block_size / sizeof(uint32_t);
    auto create_zero = [&]() -> Result<uint32_t> {
        if (!allocate) return static_cast<uint32_t>(0);
        auto b = alloc_block(); if (!b) return b.err;
        return b.value;
    };
    if (logical < 12) {
        if (ino.direct[logical] == 0) { auto b = create_zero(); if (!b || b.value == 0) return b; ino.direct[logical] = b.value; }
        return ino.direct[logical];
    }
    logical -= 12;
    auto pointer_get = [&](uint32_t ptr_block, uint32_t index) -> Result<uint32_t> {
        uint32_t v = 0;
        if (dev_.read_bytes(static_cast<uint64_t>(ptr_block) * sb_.block_size + static_cast<uint64_t>(index) * 4, &v, 4) != Err::Ok) return Err::Io;
        return v;
    };
    auto pointer_set = [&](uint32_t ptr_block, uint32_t index, uint32_t value) -> Err {
        return dev_.write_bytes(static_cast<uint64_t>(ptr_block) * sb_.block_size + static_cast<uint64_t>(index) * 4, &value, 4);
    };
    if (logical < per) {
        if (!ino.indirect) { auto b = create_zero(); if (!b || b.value == 0) return b; ino.indirect = b.value; }
        auto old = pointer_get(ino.indirect, logical); if (!old) return old.err;
        if (!old.value) { auto b = create_zero(); if (!b || b.value == 0) return b; if (pointer_set(ino.indirect, logical, b.value) != Err::Ok) return Err::Io; return b.value; }
        return old.value;
    }
    logical -= per;
    if (logical >= per * per) return Err::TooBig;
    if (!ino.double_indirect) { auto b = create_zero(); if (!b || b.value == 0) return b; ino.double_indirect = b.value; }
    uint32_t outer = logical / per, inner = logical % per;
    auto mid = pointer_get(ino.double_indirect, outer); if (!mid) return mid.err;
    uint32_t midblock = mid.value;
    if (!midblock) { auto b = create_zero(); if (!b || b.value == 0) return b; midblock = b.value; if (pointer_set(ino.double_indirect, outer, midblock) != Err::Ok) return Err::Io; }
    auto old = pointer_get(midblock, inner); if (!old) return old.err;
    if (!old.value) { auto b = create_zero(); if (!b || b.value == 0) return b; if (pointer_set(midblock, inner, b.value) != Err::Ok) return Err::Io; return b.value; }
    return old.value;
}

Err Fs::free_inode_blocks(Inode& ino) {
    for (uint32_t& b : ino.direct) if (b) { if (free_block(b) != Err::Ok) return Err::Io; b = 0; }
    const uint32_t per = sb_.block_size / sizeof(uint32_t);
    if (ino.indirect) {
        std::vector<uint32_t> ptr(per);
        if (dev_.read_block(ino.indirect, reinterpret_cast<uint8_t*>(ptr.data())) != Err::Ok) return Err::Io;
        for (uint32_t b : ptr) if (b && free_block(b) != Err::Ok) return Err::Io;
        if (free_block(ino.indirect) != Err::Ok) return Err::Io;
        ino.indirect = 0;
    }
    if (ino.double_indirect) {
        std::vector<uint32_t> outer(per);
        if (dev_.read_block(ino.double_indirect, reinterpret_cast<uint8_t*>(outer.data())) != Err::Ok) return Err::Io;
        for (uint32_t mid : outer) if (mid) {
            std::vector<uint32_t> inner(per);
            if (dev_.read_block(mid, reinterpret_cast<uint8_t*>(inner.data())) != Err::Ok) return Err::Io;
            for (uint32_t b : inner) if (b && free_block(b) != Err::Ok) return Err::Io;
            if (free_block(mid) != Err::Ok) return Err::Io;
        }
        if (free_block(ino.double_indirect) != Err::Ok) return Err::Io;
        ino.double_indirect = 0;
    }
    ino.blocks = 0; ino.size = 0;
    return Err::Ok;
}

Err Fs::read_inode_data(Inode& ino, std::string& out) {
    out.clear(); out.resize(ino.size);
    uint32_t remaining = ino.size, logical = 0, offset = 0;
    std::vector<uint8_t> block(sb_.block_size);
    while (remaining) {
        auto b = get_data_block(ino, logical++, false); if (!b) return b.err;
        if (!b.value) return Err::Corrupt;
        if (dev_.read_block(b.value, block) != Err::Ok) return Err::Io;
        uint32_t take = std::min<uint32_t>(remaining, sb_.block_size);
        std::memcpy(out.data() + offset, block.data(), take);
        offset += take; remaining -= take;
    }
    return Err::Ok;
}
Err Fs::write_inode_data(Inode& ino, const std::string& data) {
    if (data.size() > std::numeric_limits<uint32_t>::max()) return Err::TooBig;
    const uint64_t data_blocks = (data.size() + sb_.block_size - 1) / sb_.block_size;
    const uint64_t per = sb_.block_size / sizeof(uint32_t);
    const uint64_t max_blocks = 12 + per + per * per;
    if (data_blocks > max_blocks) return Err::TooBig;
    uint64_t needed = data_blocks;
    if (data_blocks > 12) ++needed; // single-indirect pointer block
    if (data_blocks > 12 + per) {
        const uint64_t double_data = data_blocks - 12 - per;
        needed += 1 + (double_data + per - 1) / per; // double root and its leaf pointer blocks
    }
    // Count the current inode's data and pointer blocks so replacing/truncating a
    // full disk file can reuse its allocation instead of failing unnecessarily.
    uint64_t old_blocks = 0;
    for (uint32_t b : ino.direct) if (b) ++old_blocks;
    if (ino.indirect) {
        ++old_blocks;
        std::vector<uint32_t> ptr(per);
        if (dev_.read_block(ino.indirect, reinterpret_cast<uint8_t*>(ptr.data())) != Err::Ok) return Err::Io;
        for (uint32_t b : ptr) if (b) ++old_blocks;
    }
    if (ino.double_indirect) {
        ++old_blocks;
        std::vector<uint32_t> outer(per);
        if (dev_.read_block(ino.double_indirect, reinterpret_cast<uint8_t*>(outer.data())) != Err::Ok) return Err::Io;
        for (uint32_t mid : outer) if (mid) {
            ++old_blocks;
            std::vector<uint32_t> inner(per);
            if (dev_.read_block(mid, reinterpret_cast<uint8_t*>(inner.data())) != Err::Ok) return Err::Io;
            for (uint32_t b : inner) if (b) ++old_blocks;
        }
    }
    if (needed > sb_.free_blocks + old_blocks) return Err::NoSpace;
    if (free_inode_blocks(ino) != Err::Ok) return Err::Io;
    uint32_t logical = 0, offset = 0;
    while (offset < data.size()) {
        auto b = get_data_block(ino, logical++, true); if (!b) return b.err;
        std::vector<uint8_t> block(sb_.block_size, 0);
        uint32_t take = static_cast<uint32_t>(std::min<size_t>(sb_.block_size, data.size() - offset));
        std::memcpy(block.data(), data.data() + offset, take);
        if (dev_.write_block(b.value, block) != Err::Ok) return Err::Io;
        ino.blocks++; offset += take;
    }
    ino.size = static_cast<uint32_t>(data.size());
    ino.mtime = ino.ctime = now_s();
    return Err::Ok;
}

Result<uint32_t> Fs::lookup(uint32_t dir_ino, const std::string& name, uint8_t* type) {
    Inode dir{}; if (Err e = read_inode(dir_ino, dir); e != Err::Ok) return e;
    if ((dir.mode & kModeMask) != kModeDirectory) return Err::NotDir;
    std::string data; if (Err e = read_inode_data(dir, data); e != Err::Ok) return e;
    for (size_t off = 0; off + sizeof(DirEntry) <= data.size(); off += sizeof(DirEntry)) {
        DirEntry ent{}; std::memcpy(&ent, data.data() + off, sizeof ent);
        if (ent.inode && ent.name_len == name.size() && name == std::string(ent.name, ent.name_len)) {
            if (type) *type = ent.type;
            return ent.inode;
        }
    }
    return Err::NotFound;
}
Result<uint32_t> Fs::resolve(const std::string& path) {
    uint32_t current = kRootIno;
    for (const auto& part : split_path(path)) {
        if (part == ".") continue;
        if (part == "..") { auto p = lookup(current, ".."); if (!p) return p.err; current = p.value; continue; }
        auto next = lookup(current, part); if (!next) return next.err;
        current = next.value;
    }
    return current;
}
Result<std::pair<uint32_t, std::string>> Fs::resolve_parent(const std::string& path) {
    auto parts = split_path(path);
    if (parts.empty()) return Err::Invalid;
    std::string name = parts.back();
    if (name == "." || name == ".." || name.size() > kNameMax) return name.size() > kNameMax ? Err::NameTooLong : Err::Invalid;
    parts.pop_back();
    std::string parent = "/";
    for (const auto& p : parts) { if (parent.size() > 1) parent += '/'; parent += p; }
    auto pino = resolve(parent); if (!pino) return pino.err;
    Inode p{}; if (Err e = read_inode(pino.value, p); e != Err::Ok) return e;
    if ((p.mode & kModeMask) != kModeDirectory) return Err::NotDir;
    return std::make_pair(pino.value, name);
}
Err Fs::add_entry(uint32_t dir_ino, const std::string& name, uint32_t child_ino, uint8_t type) {
    if (name.empty() || name.size() > kNameMax) return name.size() > kNameMax ? Err::NameTooLong : Err::Invalid;
    Inode dir{}; if (Err e = read_inode(dir_ino, dir); e != Err::Ok) return e;
    std::string data; if (Err e = read_inode_data(dir, data); e != Err::Ok) return e;
    for (size_t off = 0; off + sizeof(DirEntry) <= data.size(); off += sizeof(DirEntry)) {
        DirEntry ent{}; std::memcpy(&ent, data.data() + off, sizeof ent);
        if (ent.inode && ent.name_len == name.size() && name == std::string(ent.name, ent.name_len)) return Err::Exists;
        if (!ent.inode) {
            ent = {}; ent.inode = child_ino; ent.name_len = static_cast<uint8_t>(name.size()); ent.type = type; std::memcpy(ent.name, name.data(), name.size());
            std::memcpy(data.data() + off, &ent, sizeof ent);
            if (Err e = write_inode_data(dir, data); e != Err::Ok) return e;
            return write_inode(dir_ino, dir);
        }
    }
    DirEntry ent{}; ent.inode = child_ino; ent.name_len = static_cast<uint8_t>(name.size()); ent.type = type; std::memcpy(ent.name, name.data(), name.size());
    data.append(reinterpret_cast<const char*>(&ent), sizeof ent);
    if (Err e = write_inode_data(dir, data); e != Err::Ok) return e;
    return write_inode(dir_ino, dir);
}
Err Fs::remove_entry(uint32_t dir_ino, const std::string& name, uint32_t* removed_ino, uint8_t* type) {
    Inode dir{}; if (Err e = read_inode(dir_ino, dir); e != Err::Ok) return e;
    std::string data; if (Err e = read_inode_data(dir, data); e != Err::Ok) return e;
    for (size_t off = 0; off + sizeof(DirEntry) <= data.size(); off += sizeof(DirEntry)) {
        DirEntry ent{}; std::memcpy(&ent, data.data() + off, sizeof ent);
        if (ent.inode && ent.name_len == name.size() && name == std::string(ent.name, ent.name_len)) {
            if (removed_ino) *removed_ino = ent.inode;
            if (type) *type = ent.type;
            ent = {}; std::memcpy(data.data() + off, &ent, sizeof ent);
            if (Err e = write_inode_data(dir, data); e != Err::Ok) return e;
            return write_inode(dir_ino, dir);
        }
    }
    return Err::NotFound;
}

Err Fs::mkdir(const std::string& path) {
    if (!mounted_) return Err::Invalid;
    auto p = resolve_parent(path); if (!p) return p.err;
    if (lookup(p.value.first, p.value.second)) return Err::Exists;
    auto ino = alloc_inode(); if (!ino) return ino.err;
    auto block = alloc_block(); if (!block) { free_inode(ino.value); return block.err; }
    Inode dir{}; dir.mode = kModeDirectory | 0755; dir.links = 2; dir.size = sb_.block_size; dir.blocks = 1; dir.direct[0] = block.value; dir.atime = dir.mtime = dir.ctime = now_s();
    std::vector<uint8_t> raw(sb_.block_size, 0); DirEntry dot{}; dot.inode = ino.value; dot.name_len = 1; dot.type = kDirDirectory; std::memcpy(dot.name, ".", 1); DirEntry dotdot{}; dotdot.inode = p.value.first; dotdot.name_len = 2; dotdot.type = kDirDirectory; std::memcpy(dotdot.name, "..", 2);
    std::memcpy(raw.data(), &dot, sizeof dot); std::memcpy(raw.data() + sizeof dot, &dotdot, sizeof dotdot);
    if (dev_.write_block(block.value, raw) != Err::Ok || write_inode(ino.value, dir) != Err::Ok) return Err::Io;
    if (Err e = add_entry(p.value.first, p.value.second, ino.value, kDirDirectory); e != Err::Ok) return e;
    Inode parent{}; if (read_inode(p.value.first, parent) == Err::Ok) { parent.links++; write_inode(p.value.first, parent); }
    return Err::Ok;
}
Err Fs::touch(const std::string& path) {
    if (!mounted_) return Err::Invalid;
    auto p = resolve_parent(path); if (!p) return p.err;
    if (lookup(p.value.first, p.value.second)) return Err::Exists;
    auto ino = alloc_inode(); if (!ino) return ino.err;
    Inode file{}; file.mode = kModeRegular | 0644; file.links = 1; file.atime = file.mtime = file.ctime = now_s();
    if (Err e = write_inode(ino.value, file); e != Err::Ok) return e;
    if (Err e = add_entry(p.value.first, p.value.second, ino.value, kDirRegular); e != Err::Ok) { free_inode(ino.value); return e; }
    return Err::Ok;
}
Err Fs::write_file(const std::string& path, const std::string& data) {
    if (!mounted_) return Err::Invalid;
    auto ino = resolve(path);
    if (!ino && ino.err == Err::NotFound) { if (Err e = touch(path); e != Err::Ok) return e; ino = resolve(path); }
    if (!ino) return ino.err;
    Inode file{}; if (Err e = read_inode(ino.value, file); e != Err::Ok) return e;
    if ((file.mode & kModeMask) == kModeDirectory) return Err::IsDir;
    if (Err e = write_inode_data(file, data); e != Err::Ok) return e;
    return write_inode(ino.value, file);
}
Err Fs::append_file(const std::string& path, const std::string& data) {
    if (!mounted_) return Err::Invalid;
    auto ino = resolve(path); if (!ino) return ino.err;
    Inode file{}; if (Err e = read_inode(ino.value, file); e != Err::Ok) return e;
    if ((file.mode & kModeMask) != kModeRegular) return Err::IsDir;
    if (data.size() > std::numeric_limits<uint32_t>::max() - file.size) return Err::TooBig;
    std::string contents;
    if (Err e = read_inode_data(file, contents); e != Err::Ok) return e;
    contents.append(data);
    if (Err e = write_inode_data(file, contents); e != Err::Ok) return e;
    return write_inode(ino.value, file);
}

Err Fs::hard_link(const std::string& existing_path, const std::string& new_path) {
    if (!mounted_) return Err::Invalid;
    auto src = resolve(existing_path); if (!src) return src.err;
    Inode file{}; if (Err e = read_inode(src.value, file); e != Err::Ok) return e;
    // Like traditional Unix filesystems, this implementation does not permit
    // hard links to directories.
    if ((file.mode & kModeMask) != kModeRegular) return Err::Unsupported;
    if (file.links == std::numeric_limits<uint16_t>::max()) return Err::TooBig;
    auto dst = resolve_parent(new_path); if (!dst) return dst.err;
    if (lookup(dst.value.first, dst.value.second)) return Err::Exists;
    if (Err e = add_entry(dst.value.first, dst.value.second, src.value, kDirRegular); e != Err::Ok) return e;
    ++file.links;
    file.ctime = now_s();
    return write_inode(src.value, file);
}

Err Fs::chmod(const std::string& path, uint16_t permissions) {
    if (!mounted_) return Err::Invalid;
    if (permissions & ~uint16_t(07777)) return Err::Invalid;
    auto ino = resolve(path); if (!ino) return ino.err;
    Inode file{}; if (Err e = read_inode(ino.value, file); e != Err::Ok) return e;
    file.mode = static_cast<uint16_t>((file.mode & kModeMask) | permissions);
    file.ctime = now_s();
    return write_inode(ino.value, file);
}

Err Fs::rename_file(const std::string& old_path, const std::string& new_path) {
    if (!mounted_) return Err::Invalid;
    auto src_parent = resolve_parent(old_path); if (!src_parent) return src_parent.err;
    auto src = lookup(src_parent.value.first, src_parent.value.second); if (!src) return src.err;
    Inode file{}; if (Err e = read_inode(src.value, file); e != Err::Ok) return e;
    if ((file.mode & kModeMask) != kModeRegular) return Err::Unsupported;
    auto dst_parent = resolve_parent(new_path); if (!dst_parent) return dst_parent.err;
    if (src_parent.value.first == dst_parent.value.first && src_parent.value.second == dst_parent.value.second) return Err::Ok;
    if (lookup(dst_parent.value.first, dst_parent.value.second)) return Err::Exists;
    // Add the destination before removing the source so a failed insertion
    // cannot make the file unreachable.
    if (Err e = add_entry(dst_parent.value.first, dst_parent.value.second, src.value, kDirRegular); e != Err::Ok) return e;
    uint32_t removed = 0;
    if (Err e = remove_entry(src_parent.value.first, src_parent.value.second, &removed, nullptr); e != Err::Ok) {
        remove_entry(dst_parent.value.first, dst_parent.value.second, nullptr, nullptr);
        return e;
    }
    file.ctime = now_s();
    return write_inode(src.value, file);
}

Err Fs::truncate_file(const std::string& path, uint32_t new_size) {
    if (!mounted_) return Err::Invalid;
    auto ino = resolve(path); if (!ino) return ino.err;
    Inode file{}; if (Err e = read_inode(ino.value, file); e != Err::Ok) return e;
    if ((file.mode & kModeMask) == kModeDirectory) return Err::IsDir;
    std::string data; if (Err e = read_inode_data(file, data); e != Err::Ok) return e;
    data.resize(new_size, '\0');
    if (Err e = write_inode_data(file, data); e != Err::Ok) return e;
    return write_inode(ino.value, file);
}

Result<std::string> Fs::read_file(const std::string& path) {
    auto ino = resolve(path); if (!ino) return ino.err;
    Inode file{}; if (Err e = read_inode(ino.value, file); e != Err::Ok) return e;
    if ((file.mode & kModeMask) == kModeDirectory) return Err::IsDir;
    std::string out; if (Err e = read_inode_data(file, out); e != Err::Ok) return e;
    return out;
}
Result<std::vector<DirItem>> Fs::list(const std::string& path) {
    auto ino = resolve(path); if (!ino) return ino.err;
    Inode dir{}; if (Err e = read_inode(ino.value, dir); e != Err::Ok) return e;
    if ((dir.mode & kModeMask) != kModeDirectory) return Err::NotDir;
    std::string data; if (Err e = read_inode_data(dir, data); e != Err::Ok) return e;
    std::vector<DirItem> out;
    for (size_t off = 0; off + sizeof(DirEntry) <= data.size(); off += sizeof(DirEntry)) {
        DirEntry ent{}; std::memcpy(&ent, data.data() + off, sizeof ent);
        if (ent.inode && ent.name_len && std::string(ent.name, ent.name_len) != "." && std::string(ent.name, ent.name_len) != "..") out.push_back({ent.inode, ent.type, std::string(ent.name, ent.name_len)});
    }
    return out;
}
Err Fs::remove_file(const std::string& path) {
    auto p = resolve_parent(path); if (!p) return p.err;
    uint8_t type = 0; auto found = lookup(p.value.first, p.value.second, &type); if (!found) return found.err;
    if (type == kDirDirectory) return Err::IsDir;
    uint32_t ino = 0; if (Err e = remove_entry(p.value.first, p.value.second, &ino, nullptr); e != Err::Ok) return e;
    Inode file{}; if (Err e = read_inode(ino, file); e != Err::Ok) return e;
    if (file.links > 1) {
        --file.links;
        file.ctime = now_s();
        return write_inode(ino, file);
    }
    if (Err e = free_inode_blocks(file); e != Err::Ok) return e;
    return free_inode(ino);
}
Err Fs::remove_dir(const std::string& path) {
    auto p = resolve_parent(path); if (!p) return p.err;
    auto found = lookup(p.value.first, p.value.second); if (!found) return found.err;
    Inode dir{}; if (Err e = read_inode(found.value, dir); e != Err::Ok) return e;
    if ((dir.mode & kModeMask) != kModeDirectory) return Err::NotDir;
    auto entries = list(path); if (!entries) return entries.err;
    if (!entries.value.empty()) return Err::NotEmpty;
    uint32_t removed = 0; if (Err e = remove_entry(p.value.first, p.value.second, &removed); e != Err::Ok) return e;
    if (Err e = free_inode_blocks(dir); e != Err::Ok) return e;
    if (Err e = free_inode(removed); e != Err::Ok) return e;
    Inode parent{}; if (read_inode(p.value.first, parent) == Err::Ok && parent.links > 0) { parent.links--; write_inode(p.value.first, parent); }
    return Err::Ok;
}
Result<StatInfo> Fs::stat(const std::string& path) {
    auto ino = resolve(path); if (!ino) return ino.err;
    Inode in{}; if (Err e = read_inode(ino.value, in); e != Err::Ok) return e;
    return StatInfo{ino.value, in.mode, in.links, in.size, in.blocks, in.mtime};
}
SpaceInfo Fs::space() const { return {sb_.total_blocks, sb_.free_blocks, sb_.total_inodes, sb_.free_inodes}; }

Err Fs::check() {
    if (!mounted_) return Err::Invalid;
    if (validate_superblock(sb_, dev_.size_bytes()) != Err::Ok) return Err::Corrupt;
    if (block_bitmap_.count_free() != sb_.free_blocks || inode_bitmap_.count_free() != sb_.free_inodes) return Err::Corrupt;
    if (!inode_bitmap_.test(kRootIno - 1) || !block_bitmap_.test(sb_.data_start)) return Err::Corrupt;

    // Every allocated data-region block must be owned by exactly one inode or pointer tree.
    std::vector<uint8_t> referenced(sb_.total_blocks, 0);
    auto mark_block = [&](uint32_t b) -> bool {
        if (!is_data_block(b) || !block_bitmap_.test(b) || referenced[b]) return false;
        referenced[b] = 1;
        return true;
    };
    const uint32_t per = sb_.block_size / sizeof(uint32_t);
    for (uint32_t ino = 1; ino <= sb_.total_inodes; ++ino) {
        if (!inode_bitmap_.test(ino - 1)) continue;
        Inode in{};
        if (read_inode(ino, in) != Err::Ok) return Err::Corrupt;
        if (ino > 1 && in.mode == 0) return Err::Corrupt;
        uint32_t data_count = 0;
        for (uint32_t b : in.direct) if (b) { if (!mark_block(b)) return Err::Corrupt; ++data_count; }
        auto check_pointer_block = [&](uint32_t ptr_block, bool double_level) -> bool {
            if (!ptr_block) return true;
            if (!mark_block(ptr_block)) return false;
            std::vector<uint32_t> ptr(per);
            if (dev_.read_block(ptr_block, reinterpret_cast<uint8_t*>(ptr.data())) != Err::Ok) return false;
            for (uint32_t b : ptr) {
                if (!b) continue;
                if (double_level) {
                    if (!mark_block(b)) return false;
                    std::vector<uint32_t> inner(per);
                    if (dev_.read_block(b, reinterpret_cast<uint8_t*>(inner.data())) != Err::Ok) return false;
                    for (uint32_t data : inner) if (data) { if (!mark_block(data)) return false; ++data_count; }
                } else {
                    if (!mark_block(b)) return false;
                    ++data_count;
                }
            }
            return true;
        };
        if (!check_pointer_block(in.indirect, false) || !check_pointer_block(in.double_indirect, true)) return Err::Corrupt;
        if (data_count != in.blocks) return Err::Corrupt;
        if ((in.mode & kModeMask) == kModeDirectory) {
            std::string bytes;
            if (read_inode_data(in, bytes) != Err::Ok || bytes.size() % sizeof(DirEntry) != 0) return Err::Corrupt;
            for (size_t off = 0; off + sizeof(DirEntry) <= bytes.size(); off += sizeof(DirEntry)) {
                DirEntry ent{}; std::memcpy(&ent, bytes.data() + off, sizeof ent);
                if (!ent.inode) continue;
                if (ent.inode > sb_.total_inodes || !inode_bitmap_.test(ent.inode - 1) || ent.name_len == 0 || ent.name_len > kNameMax) return Err::Corrupt;
            }
        }
    }
    for (uint32_t b = 0; b < sb_.total_blocks; ++b) {
        if (is_data_block(b) && block_bitmap_.test(b) != (referenced[b] != 0)) return Err::Corrupt;
    }
    for (uint32_t g = 0; g < groups_.size(); ++g) {
        uint32_t fb = 0, fi = 0;
        const auto& gd = groups_[g];
        for (uint32_t b = gd.first_block; b < gd.first_block + gd.blocks_count; ++b) if (is_data_block(b) && !block_bitmap_.test(b)) ++fb;
        uint32_t first_ino = g * sb_.inodes_per_group;
        for (uint32_t i = first_ino; i < first_ino + gd.inodes_count; ++i) if (!inode_bitmap_.test(i)) ++fi;
        if (fb != gd.free_blocks || fi != gd.free_inodes) return Err::Corrupt;
    }
    return Err::Ok;
}
} // namespace edufs
