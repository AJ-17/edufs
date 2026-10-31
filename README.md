# EduFS 0.3.0

EduFS is an educational, ext2-inspired filesystem in C++20. It stores filesystem metadata and file contents in a disk-image file. It is **not** a byte-compatible ext2 implementation and cannot be mounted by the Linux kernel.

## Build and test

```bash
cmake -S . -B build -DEDUFS_SANITIZE=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

## Quick demo

```bash
./build/edufs format demo.img --size 8M
./build/edufs demo.img mkdir -p /home/anshu/docs
./build/edufs demo.img write /home/anshu/docs/notes.txt 'EduFS is growing'
./build/edufs demo.img append /home/anshu/docs/notes.txt ' with append support'
./build/edufs demo.img ln /home/anshu/docs/notes.txt /home/anshu/docs/notes-link.txt
./build/edufs demo.img chmod 0640 /home/anshu/docs/notes.txt
./build/edufs demo.img ls -l /home/anshu/docs
./build/edufs demo.img stat /home/anshu/docs/notes-link.txt
./build/edufs demo.img mv /home/anshu/docs/notes.txt /home/anshu/docs/renamed.txt
./build/edufs demo.img cat /home/anshu/docs/notes-link.txt
./build/edufs demo.img find /home
./build/edufs demo.img tree /home
./build/edufs demo.img du /home
./build/edufs demo.img df
./build/edufs demo.img fsck
```

Each CLI invocation mounts and cleanly unmounts the image, so commands also exercise persistence across process restarts.

## Commands

- **Creation:** `format IMAGE`, `mkdir PATH`, `mkdir -p PATH`, `touch PATH`
- **Read/write:** `cat PATH`, `write PATH TEXT`, `append PATH TEXT`, `truncate PATH [SIZE]`, `cp SOURCE DESTINATION`
- **Directory/file operations:** `ls [PATH]`, `ls -l [PATH]`, `rm PATH`, `rmdir PATH`, `mv SOURCE DESTINATION`, `ln EXISTING_FILE NEW_LINK`
- **Metadata and inspection:** `stat PATH`, `chmod OCTAL_MODE PATH`, `df`, `du PATH`, `find PATH`, `tree PATH`, `blockmap PATH`, `dump-dir PATH`, `dump-inode INODE`, `dump-bitmap blocks|inodes [FIRST] [COUNT]`, `dump-super`
- **Integrity:** `fsck`

`ln` creates hard links to regular files. The linked names share one inode and data payload; removing a name decrements the inode link count, and data blocks are freed only after the last hard link is removed. `mv` currently renames/moves regular files only; directory moves are deliberately unsupported. `cp` copies file contents and overwrites an existing destination file.

## Disk format and limitations

- Little-endian host only; block sizes 1024, 2048, and 4096 bytes.
- Per-group block/inode bitmaps and inode tables with a compact group descriptor table.
- 128-byte inodes with 12 direct pointers, single-indirect and double-indirect pointers.
- Fixed 64-byte directory records; maximum name length 56 bytes.
- Permission bits can be stored and changed with `chmod`, but access permissions are **not enforced**.
- No symbolic links, journaling, crash recovery, extents, FUSE/kernel driver, or physical-device backend.
- The custom on-disk structures are ext2-inspired, not ext2-compatible. Do not use `mke2fs`/`e2fsck` on EduFS images.
- `fsck` performs structural checks on superblock counts, bitmaps, inode/block ownership, pointers, and directory references; it is not a production filesystem checker.

## Safety

Use disposable image files. Formatting overwrites the selected image. Physical-device support is not implemented.
