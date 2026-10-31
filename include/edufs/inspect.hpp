#pragma once
#include <cstdint>
#include <string>
#include "edufs/fs.hpp"
namespace edufs {
std::string dump_super(Fs& fs);
std::string format_time(uint32_t t);
Result<std::string> dump_inode(Fs& fs, uint32_t ino);
std::string dump_bitmap(Fs& fs, bool inode_bitmap, uint32_t first = 0, uint32_t count = 128);
Result<std::string> dump_directory(Fs& fs, const std::string& path);
Result<std::string> dump_blockmap(Fs& fs, const std::string& path);
}
