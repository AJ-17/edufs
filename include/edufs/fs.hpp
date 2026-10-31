#pragma once
// Fs: one object = one mounted disk image.
#include <cstdint>
#include <string>
#include <utility>

#include "edufs/bitmap.hpp"
#include <vector>

#include "edufs/block_device.hpp"
#include "edufs/errors.hpp"
#include "edufs/inode.hpp"
#include "edufs/layout.hpp"
#include "edufs/superblock.hpp"

namespace edufs {
struct DirItem { uint32_t inode; uint8_t type; std::string name; };
struct StatInfo { uint32_t inode; uint16_t mode; uint16_t links; uint32_t size; uint32_t blocks; uint32_t mtime; };
struct SpaceInfo { uint32_t total_blocks; uint32_t free_blocks; uint32_t total_inodes; uint32_t free_inodes; };

class Fs {
public:
    Fs() = default;
    ~Fs();
    Fs(const Fs&) = delete;
    Fs& operator=(const Fs&) = delete;

    static Err format(const std::string& path, const FormatOptions& opt = {});
    Err mount(const std::string& path);
    Err unmount();
    bool mounted() const { return mounted_; }
    bool was_clean() const { return was_clean_; }
    const Superblock& super() const { return sb_; }
    BlockDevice& device() { return dev_; }

    Err mkdir(const std::string& path);
    Err touch(const std::string& path);
    Err write_file(const std::string& path, const std::string& data);
    Err append_file(const std::string& path, const std::string& data);
    Err hard_link(const std::string& existing_path, const std::string& new_path);
    Err chmod(const std::string& path, uint16_t permissions);
    Err rename_file(const std::string& old_path, const std::string& new_path);
    Err truncate_file(const std::string& path, uint32_t new_size = 0);
    Result<std::string> read_file(const std::string& path);
    Result<std::vector<DirItem>> list(const std::string& path);
    Err remove_file(const std::string& path);
    Err remove_dir(const std::string& path);
    Result<StatInfo> stat(const std::string& path);
    SpaceInfo space() const;
    Err check();

private:
    Err write_super();
    Err load_groups();
    Err save_groups();
    Err load_bitmaps();
    Err save_block_bitmap();
    Err save_inode_bitmap();
    Result<uint32_t> alloc_block();
    Err free_block(uint32_t block);
    bool is_data_block(uint32_t block) const;
    Result<uint32_t> alloc_inode();
    Err free_inode(uint32_t ino);
    Err read_inode(uint32_t ino, Inode& out);
    Err write_inode(uint32_t ino, const Inode& in);
    Result<uint32_t> get_data_block(Inode& ino, uint32_t logical, bool allocate);
    Err free_inode_blocks(Inode& ino);
    Result<uint32_t> resolve(const std::string& path);
    Result<std::pair<uint32_t, std::string>> resolve_parent(const std::string& path);
    Result<uint32_t> lookup(uint32_t dir_ino, const std::string& name, uint8_t* type = nullptr);
    Err add_entry(uint32_t dir_ino, const std::string& name, uint32_t child_ino, uint8_t type);
    Err remove_entry(uint32_t dir_ino, const std::string& name, uint32_t* removed_ino = nullptr, uint8_t* type = nullptr);
    Err read_inode_data(Inode& ino, std::string& out);
    Err write_inode_data(Inode& ino, const std::string& data);

    BlockDevice dev_;
    Superblock sb_{};
    std::vector<GroupDesc> groups_;
    Bitmap block_bitmap_;
    Bitmap inode_bitmap_;
    bool mounted_ = false;
    bool was_clean_ = true;
};
}  // namespace edufs
