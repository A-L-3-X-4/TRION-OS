#!/usr/bin/env bash

set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p build
export MTOOLS_SKIP_CHECK=1

rm -f build/disk.img build/scratch.img build/disk4k.img build/scratch2k.img
truncate -s 64M build/disk.img
mkfs.fat -F 32 -n INFDISK -s 1 build/disk.img >/dev/null

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
printf 'hello from the host\n' > "$T/HELLO.TXT"
printf 'long names, with spaces\n' > "$T/Long File Name Example.txt"
printf 'read only\n' > "$T/LOCKED.TXT"
printf 'lower\n' > "$T/lower.txt"
python3 - "$T/BIG.BIN" <<'PY'
import sys
n = 3*1024*1024 + 777
open(sys.argv[1], 'wb').write(bytes(((i ^ (i >> 8) ^ (i >> 16)) * 167 + 13) & 0xFF for i in range(n)))
PY
mcopy -i build/disk.img "$T/HELLO.TXT" "$T/Long File Name Example.txt" "$T/LOCKED.TXT" "$T/lower.txt" "$T/BIG.BIN" ::
mattrib -i build/disk.img +r ::LOCKED.TXT
mmd -i build/disk.img ::DOCS ::DOCS/NESTED
printf "marker: the kernel may write to this image once\n" > "$T/KERNEL.TST"
mcopy -i build/disk.img "$T/KERNEL.TST" ::
printf 'deep\n' > "$T/DEEP.TXT"
mcopy -i build/disk.img "$T/DEEP.TXT" ::DOCS/NESTED/

truncate -s 40M build/scratch.img
printf 'INFINITYOS-FSTEST-SCRATCH\0' | dd of=build/scratch.img conv=notrunc status=none

truncate -s 288M build/disk4k.img
mkfs.fat -F 32 -S 4096 -s 1 -n DISK4K build/disk4k.img >/dev/null
printf 'hello from a 4096-byte-sector volume\n' > "$T/HELLO4K.TXT"
mcopy -i build/disk4k.img "$T/HELLO4K.TXT" ::HELLO.TXT
mcopy -i build/disk4k.img "$T/Long File Name Example.txt" "$T/BIG.BIN" "$T/KERNEL.TST" ::
mmd -i build/disk4k.img ::DOCS ::DOCS/NESTED
mcopy -i build/disk4k.img "$T/DEEP.TXT" ::DOCS/NESTED/

truncate -s 160M build/scratch2k.img
printf 'INFINITYOS-FSTEST-SCRATCH\0' | dd of=build/scratch2k.img conv=notrunc status=none
echo "disks ready: build/disk.img build/scratch.img build/disk4k.img build/scratch2k.img"
