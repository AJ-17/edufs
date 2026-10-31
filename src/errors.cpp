#include "edufs/errors.hpp"

namespace edufs {

const char* err_str(Err e) {
    switch (e) {
        case Err::Ok: return "ok";
        case Err::Io: return "I/O error";
        case Err::BadMagic: return "not an EduFS image (bad magic number)";
        case Err::Corrupt: return "filesystem metadata is corrupt";
        case Err::Invalid: return "invalid argument";
        case Err::NoSpace: return "no space left on device";
        case Err::NoInodes: return "no free inodes";
        case Err::NotFound: return "no such file or directory";
        case Err::Exists: return "already exists";
        case Err::NotDir: return "not a directory";
        case Err::IsDir: return "is a directory";
        case Err::NotEmpty: return "directory not empty";
        case Err::NameTooLong: return "name too long";
        case Err::TooBig: return "file too large";
        case Err::Unsupported: return "unsupported";
    }
    return "unknown error";
}

}  // namespace edufs