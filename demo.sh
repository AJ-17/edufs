#!/usr/bin/env bash
set -euo pipefail
BIN="${1:-./build/edufs}"
IMG="${2:-edufs-demo.img}"
"$BIN" format "$IMG" --size 8M
"$BIN" "$IMG" fsck
"$BIN" "$IMG" mkdir -p /home/anshu/docs
"$BIN" "$IMG" write /home/anshu/docs/notes.txt 'EduFS is an educational filesystem'
"$BIN" "$IMG" append /home/anshu/docs/notes.txt ' with ext2-inspired features'
"$BIN" "$IMG" ln /home/anshu/docs/notes.txt /home/anshu/docs/notes-link.txt
"$BIN" "$IMG" chmod 0640 /home/anshu/docs/notes.txt
"$BIN" "$IMG" ls -l /home/anshu/docs
"$BIN" "$IMG" mv /home/anshu/docs/notes.txt /home/anshu/docs/renamed.txt
"$BIN" "$IMG" cat /home/anshu/docs/notes-link.txt
"$BIN" "$IMG" find /home
"$BIN" "$IMG" tree /home
"$BIN" "$IMG" du /home
"$BIN" "$IMG" blockmap /home/anshu/docs/renamed.txt
"$BIN" "$IMG" df
"$BIN" "$IMG" fsck
