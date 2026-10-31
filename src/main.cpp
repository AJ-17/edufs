// EduFS command-line tool. Commands operate on a disk-image file.
#include <iostream>
#include <functional>
#include <iomanip>
#include <string>
#include <vector>
#include "edufs/fs.hpp"
#include "edufs/inspect.hpp"
#include "edufs/version.hpp"
using namespace edufs;

static void usage() {
    std::cerr << "EduFS " << version() << "\n\n"
                 "Usage:\n"
                 "  edufs format <image> [--size 8M] [--block-size 1024] [--inodes N]\n"
                 "  edufs <image> dump-super\n"
                 "  edufs <image> mkdir <path> | mkdir -p <path>\n"
                 "  edufs <image> ls [-l] [path]\n"
                 "  edufs <image> touch <path>\n"
                 "  edufs <image> write <path> <text> | append <path> <text>\n"
                 "  edufs <image> cp <source> <destination> | mv <source> <destination>\n"
                 "  edufs <image> ln <existing-file> <new-hard-link>\n"
                 "  edufs <image> chmod <octal-mode> <path>\n"
                 "  edufs <image> find <path> | tree <path> | du <path>\n"
                 "  edufs <image> cat <path>\n"
                 "  edufs <image> rm <path> | rmdir <path> | truncate <path> [size]\n"
                 "  edufs <image> stat <path> | df | fsck\n"
                 "  edufs <image> dump-inode <number> | dump-bitmap <blocks|inodes> [first] [count]\n"
                 "  edufs <image> dump-dir <path> | blockmap <path>\n";
}
static bool parse_size(const std::string& s, uint64_t& out) {
    try {
        size_t pos = 0; uint64_t v = std::stoull(s, &pos);
        if (pos < s.size()) {
            if (pos + 1 != s.size()) return false;
            switch (s[pos]) { case 'K': case 'k': v <<= 10; break; case 'M': case 'm': v <<= 20; break; case 'G': case 'g': v <<= 30; break; default: return false; }
        }
        out = v; return true;
    } catch (...) { return false; }
}
static int do_format(const std::vector<std::string>& a) {
    if (a.size() < 3) { usage(); return 2; }
    FormatOptions opt;
    for (size_t i = 3; i < a.size(); i += 2) {
        if (i + 1 >= a.size()) { usage(); return 2; }
        try {
            if (a[i] == "--size") { if (!parse_size(a[i+1], opt.size_bytes)) { std::cerr << "bad --size\n"; return 2; } }
            else if (a[i] == "--block-size") opt.block_size = static_cast<uint32_t>(std::stoul(a[i+1]));
            else if (a[i] == "--inodes") opt.inodes = static_cast<uint32_t>(std::stoul(a[i+1]));
            else { usage(); return 2; }
        } catch (...) { std::cerr << "bad value for " << a[i] << "\n"; return 2; }
    }
    if (Err e = Fs::format(a[2], opt); e != Err::Ok) { std::cerr << "format: " << err_str(e) << "\n"; return 1; }
    std::cout << "Formatted " << a[2] << " successfully.\n";
    return 0;
}
int main(int argc, char** argv) {
    std::vector<std::string> a(argv, argv + argc);
    if (a.size() < 2 || a[1] == "-h" || a[1] == "--help") { usage(); return a.size() < 2 ? 2 : 0; }
    if (a[1] == "format") return do_format(a);
    if (a.size() < 3) { usage(); return 2; }
    Fs fs;
    if (Err e = fs.mount(a[1]); e != Err::Ok) { std::cerr << "cannot mount " << a[1] << ": " << err_str(e) << "\n"; return 1; }
    if (!fs.was_clean()) std::cerr << "warning: image was not cleanly unmounted\n";
    const std::string cmd = a[2];
    auto need_path = [&]() { if (a.size() < 4) { usage(); return false; } return true; };
    Err e = Err::Ok;
    if (cmd == "dump-super") std::cout << dump_super(fs);
    else if (cmd == "mkdir") {
        if (!need_path()) return 2;
        if (a[3] != "-p") e = fs.mkdir(a[3]);
        else {
            if (a.size() < 5) { usage(); return 2; }
            const std::string target = a[4]; std::string prefix = "/";
            size_t pos = 0;
            while (pos < target.size()) {
                while (pos < target.size() && target[pos] == '/') ++pos;
                if (pos == target.size()) break;
                size_t end = target.find('/', pos); if (end == std::string::npos) end = target.size();
                const std::string part = target.substr(pos, end - pos);
                if (prefix.size() > 1) prefix += '/';
                prefix += part;
                auto st = fs.stat(prefix);
                if (!st) { if (st.err != Err::NotFound) { e = st.err; break; } e = fs.mkdir(prefix); if (e != Err::Ok) break; }
                else if ((st.value.mode & kModeMask) != kModeDirectory) { e = Err::NotDir; break; }
                pos = end;
            }
        }
    }
    else if (cmd == "touch") { if (!need_path()) return 2; e = fs.touch(a[3]); }
    else if (cmd == "write") { if (a.size() < 5) { usage(); return 2; } e = fs.write_file(a[3], a[4]); }
    else if (cmd == "append") { if (a.size() < 5) { usage(); return 2; } e = fs.append_file(a[3], a[4]); }
    else if (cmd == "ln") { if (a.size() < 5) { usage(); return 2; } e = fs.hard_link(a[3], a[4]); }
    else if (cmd == "chmod") {
        if (a.size() < 5) { usage(); return 2; }
        try { size_t pos = 0; unsigned long mode = std::stoul(a[3], &pos, 8); if (pos != a[3].size() || mode > 07777) e = Err::Invalid; else e = fs.chmod(a[4], static_cast<uint16_t>(mode)); }
        catch (...) { e = Err::Invalid; }
    }
    else if (cmd == "cp") {
        if (a.size() < 5) { usage(); return 2; }
        auto data = fs.read_file(a[3]); if (!data) e = data.err; else e = fs.write_file(a[4], data.value);
    }
    else if (cmd == "mv") { if (a.size() < 5) { usage(); return 2; } e = fs.rename_file(a[3], a[4]); }
    else if (cmd == "cat") { if (!need_path()) return 2; auto r = fs.read_file(a[3]); if (!r) e = r.err; else std::cout << r.value << (r.value.empty() || r.value.back() == '\n' ? "" : "\n"); }
    else if (cmd == "ls") {
        const bool long_format = a.size() >= 4 && a[3] == "-l";
        const std::string path = long_format ? (a.size() >= 5 ? a[4] : "/") : (a.size() >= 4 ? a[3] : "/");
        auto r = fs.list(path);
        if (!r) e = r.err;
        else for (const auto& x : r.value) {
            if (!long_format) std::cout << (x.type == kDirDirectory ? "d " : "- ") << x.inode << "\t" << x.name << "\n";
            else {
                const std::string child = path == "/" ? "/" + x.name : path + "/" + x.name;
                auto st = fs.stat(child); if (!st) { e = st.err; break; }
                std::cout << ((st.value.mode & kModeMask) == kModeDirectory ? 'd' : '-')
                          << std::oct << std::setw(4) << std::setfill('0') << (st.value.mode & 07777) << std::dec << std::setfill(' ')
                          << " " << st.value.links << " " << st.value.size << " " << st.value.inode << " " << x.name << "\n";
            }
        }
    }
    else if (cmd == "rm") { if (!need_path()) return 2; e = fs.remove_file(a[3]); }
    else if (cmd == "rmdir") { if (!need_path()) return 2; e = fs.remove_dir(a[3]); }
    else if (cmd == "truncate") { if (!need_path()) return 2; uint32_t n = 0; try { if (a.size() > 4) n = static_cast<uint32_t>(std::stoul(a[4])); } catch (...) { e = Err::Invalid; } if (e == Err::Ok) e = fs.truncate_file(a[3], n); }
    else if (cmd == "stat") { if (!need_path()) return 2; auto r = fs.stat(a[3]); if (!r) e = r.err; else std::cout << "inode: " << r.value.inode << "\nmode: " << std::oct << r.value.mode << std::dec << "\nlinks: " << r.value.links << "\nsize: " << r.value.size << " bytes\nblocks: " << r.value.blocks << "\nmodified: " << format_time(r.value.mtime) << "\n"; }
    else if (cmd == "df") { auto s = fs.space(); std::cout << "Blocks: " << s.free_blocks << " free / " << s.total_blocks << " total\nInodes: " << s.free_inodes << " free / " << s.total_inodes << " total\n"; }
    else if (cmd == "fsck") { e = fs.check(); if (e == Err::Ok) std::cout << "EduFS check: clean\n"; }
    else if (cmd == "dump-inode") { if (!need_path()) return 2; try { auto r = dump_inode(fs, static_cast<uint32_t>(std::stoul(a[3]))); if (!r) e = r.err; else std::cout << r.value; } catch (...) { e = Err::Invalid; } }
    else if (cmd == "dump-bitmap") { if (a.size() < 4 || (a[3] != "blocks" && a[3] != "inodes")) { usage(); return 2; } uint32_t first = 0, count = 128; try { if (a.size() > 4) first = static_cast<uint32_t>(std::stoul(a[4])); if (a.size() > 5) count = static_cast<uint32_t>(std::stoul(a[5])); } catch (...) { return 2; } std::cout << dump_bitmap(fs, a[3] == "inodes", first, count); }
    else if (cmd == "dump-dir") { if (!need_path()) return 2; auto r = dump_directory(fs, a[3]); if (!r) e = r.err; else std::cout << r.value; }
    else if (cmd == "blockmap") { if (!need_path()) return 2; auto r = dump_blockmap(fs, a[3]); if (!r) e = r.err; else std::cout << r.value; }
    else if (cmd == "find" || cmd == "tree" || cmd == "du") {
        const std::string root = a.size() >= 4 ? a[3] : "/";
        auto root_stat = fs.stat(root); if (!root_stat) e = root_stat.err;
        else {
            std::function<uint64_t(const std::string&, unsigned)> walk;
            walk = [&](const std::string& path, unsigned depth) -> uint64_t {
                auto st = fs.stat(path); if (!st) { e = st.err; return 0; }
                uint64_t total = st.value.size;
                if (cmd == "find") std::cout << path << "\n";
                if ((st.value.mode & kModeMask) != kModeDirectory) return total;
                auto entries = fs.list(path); if (!entries) { e = entries.err; return 0; }
                for (const auto& item : entries.value) {
                    const std::string child = path == "/" ? "/" + item.name : path + "/" + item.name;
                    if (cmd == "tree") std::cout << std::string((depth + 1) * 2, ' ') << item.name << (item.type == kDirDirectory ? "/" : "") << "\n";
                    total += walk(child, depth + 1);
                }
                return total;
            };
            if (cmd == "tree") std::cout << root << (((root_stat.value.mode & kModeMask) == kModeDirectory) ? "/\n" : "\n");
            uint64_t total = walk(root, 0);
            if (cmd == "du" && e == Err::Ok) std::cout << total << "\t" << root << "\n";
        }
    }
    else { usage(); return 2; }
    if (e != Err::Ok) { std::cerr << cmd << ": " << err_str(e) << "\n"; return 1; }
    return 0;
}
