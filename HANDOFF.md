# EduFS 0.3.0 — implementation handoff

## Baseline retained

The project retains the existing C++20 block-device layer, on-disk superblock/group descriptors, block and inode bitmaps, 128-byte inodes, direct/single-indirect/double-indirect block mapping, fixed-size directory records, path resolution, formatting/mounting, inspection commands, and structural `fsck` checks.

## New user-facing capabilities in 0.3.0

- `append PATH TEXT`: append bytes without requiring the caller to manually read and rewrite the entire file.
- `ln EXISTING_FILE NEW_LINK`: create a hard link to a regular file; directory hard links are rejected.
- Correct unlink behavior for hard links: decrement the inode link count and preserve contents until the final name is removed.
- `chmod OCTAL_MODE PATH`: update inode permission bits. This stores metadata only; permission enforcement is not implemented.
- `mv SOURCE DESTINATION`: rename/move a regular file by changing directory entries without copying file data. Directory moves are unsupported.
- `cp SOURCE DESTINATION`: copy file bytes, overwriting an existing destination regular file.
- `mkdir -p PATH`: create missing directory components.
- `ls -l [PATH]`: display type, mode, link count, size, inode number and name.
- Recursive `find PATH`, `tree PATH`, and `du PATH` CLI commands.
- Version bumped to 0.3.0 and README updated with command examples and limitations.

## Implementation notes / caveats

- Hard links refer to one inode and therefore share contents and metadata.
- Hard-link count is a 16-bit field; creation rejects overflow.
- `mv` only supports regular files, and destination must not already exist.
- `cp` is implemented at the CLI layer using read and write operations; it is not an atomic copy.
- `chmod` does not enforce read/write/execute permissions.
- No journaling or atomic multi-operation transactions are provided. A power loss or I/O error during a multi-step mutation can still leave inconsistent metadata.
- EduFS remains a custom educational format; it is not mountable and is not binary-compatible with ext2.

## Verify locally

```bash
cmake -S . -B build -DEDUFS_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/edufs format demo.img --size 8M
./build/edufs demo.img mkdir -p /docs/nested
./build/edufs demo.img write /docs/nested/a.txt 'hello'
./build/edufs demo.img append /docs/nested/a.txt ' world'
./build/edufs demo.img ln /docs/nested/a.txt /docs/alias.txt
./build/edufs demo.img ls -l /docs
./build/edufs demo.img mv /docs/nested/a.txt /docs/nested/b.txt
./build/edufs demo.img fsck
```
