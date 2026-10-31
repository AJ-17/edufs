#pragma once
// Error codes and a tiny Result<T> used by every EduFS layer.
#include <utility>

namespace edufs {

enum class Err {
    Ok = 0,
    Io,           // host file read/write failed or out of range access
    BadMagic,     // not an EduFS image
    Corrupt,      // metadata is inconsistent
    Invalid,      // bad argument
    NoSpace,      // no free data blocks
    NoInodes,     // no free inodes
    NotFound,
    Exists,
    NotDir,
    IsDir,
    NotEmpty,
    NameTooLong,
    TooBig,       // file would exceed the maximum size
    Unsupported,
};

const char* err_str(Err e);

// A value or an error. Implicitly constructible from either, so functions can
// simply `return Err::NotFound;` or `return some_value;`.
template <class T>
struct Result {
    Err err = Err::Ok;
    T value{};

    Result() = default;
    Result(Err e) : err(e) {}
    Result(T v) : value(std::move(v)) {}

    bool ok() const { return err == Err::Ok; }
    explicit operator bool() const { return ok(); }
};

}  // namespace edufs