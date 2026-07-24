#!/bin/sh
# Create and format the FAT32 disk image the kernel reads, and stock it with the
# test files the filesystem self-test expects.
#
# Idempotent by design: if the image already exists this exits without touching
# it. Reformatting would silently destroy anything the disk had accumulated, and
# `make run` calls into here on every boot.
#
# No sudo, no mounting. mtools writes into the image file directly, treating it
# as a bare FAT volume, so formatting and copying need no privileges and no
# loopback device. Install it with `brew install mtools` (macOS) or
# `apt install mtools` (Debian/Ubuntu).
set -e

IMG="$1"
SIZE="$2"

if [ -z "$IMG" ] || [ -z "$SIZE" ]; then
    echo "usage: $0 <image-path> <size, e.g. 64M>" >&2
    exit 1
fi

if [ -f "$IMG" ]; then
    echo "$IMG already exists, leaving it alone (delete it to reformat)"
    exit 0
fi

if ! command -v mformat >/dev/null 2>&1; then
    echo "error: mtools not found (need mformat/mcopy)." >&2
    echo "  macOS:  brew install mtools" >&2
    echo "  Debian: sudo apt install mtools" >&2
    exit 1
fi

# The three test files, with contents the kernel self-test knows byte for byte.
# BIG.TXT is deliberately larger than one cluster so that reading it genuinely
# exercises FAT chain following. A single-cluster file can be read correctly by
# code whose chain logic is completely broken, so it proves nothing on its own.
HELLO_TEXT="Hello from FAT32!"
TEST_TEXT="MiniOS reads files."
BIG_PATTERN="0123456789ABCDEF"   # repeated BIG_REPEATS times, no newlines
BIG_REPEATS=1024                 # 1024 * 16 = 16384 bytes

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

printf '%s' "$HELLO_TEXT" > "$STAGE/HELLO.TXT"
printf '%s' "$TEST_TEXT"  > "$STAGE/TEST.TXT"
awk -v pattern="$BIG_PATTERN" -v n="$BIG_REPEATS" \
    'BEGIN { for (i = 0; i < n; i++) printf "%s", pattern }' > "$STAGE/BIG.TXT"

# A raw image is a flat array of 512-byte blocks, exactly what the ATA driver
# expects. qemu-img is used rather than dd because qemu is already a dependency
# and the size and format are stated explicitly.
qemu-img create -f raw "$IMG" "$SIZE" >/dev/null

# -F forces FAT32 rather than letting mtools pick FAT12/FAT16 by volume size.
# The volume starts at block 0 (a "superfloppy", no partition table), so block 0
# of the disk is the FAT32 boot sector, which is what fat32_init reads.
mformat -i "$IMG" -F ::

mcopy -i "$IMG" "$STAGE/HELLO.TXT" ::/
mcopy -i "$IMG" "$STAGE/TEST.TXT"  ::/
mcopy -i "$IMG" "$STAGE/BIG.TXT"   ::/

echo "Formatted $IMG ($SIZE, FAT32) with:"
mdir -i "$IMG" ::
