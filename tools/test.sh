#!/usr/bin/env bash

set -euo pipefail
cd "$(dirname "$0")/.."

ISO=build/infinityos.iso
WAIT_FOR=${WAIT_FOR:-'W32TEST (PASS|FAIL)|KERNEL FAULT|KERNEL PANIC'}
SOCK=build/mon.sock
WAIT_SECS=${WAIT_SECS:-240}

rm -f build/serial.log build/qemu.log build/screen.ppm build/screen.png "$SOCK"

CDROM=(-drive file="$ISO",if=none,id=cd,media=cdrom,format=raw -device ich9-ahci,id=ahci
       -device ide-cd,drive=cd,bus=ahci.0,bootindex=0)
DISKS=()
[ -f build/disk.img ] && DISKS+=(-drive file=build/disk.img,format=raw,if=ide,index=0)
[ -f build/scratch.img ] && DISKS+=(-drive file=build/scratch.img,format=raw,if=ide,index=1)
[ -f build/disk4k.img ] && DISKS+=(-drive file=build/disk4k.img,format=raw,if=ide,index=2)
[ -f build/scratch2k.img ] && DISKS+=(-drive file=build/scratch2k.img,format=raw,if=ide,index=3)

qemu-system-x86_64 "${CDROM[@]}" "${DISKS[@]}" -m 256M -display none -vga std \
    -serial file:build/serial.log \
    -monitor "unix:$SOCK,server,nowait" \
    -no-reboot -d int,cpu_reset -D build/qemu.log "$@" &
QPID=$!
trap 'kill $QPID 2>/dev/null || true' EXIT

for _ in $(seq 1 $((WAIT_SECS * 5))); do
    if grep -qE "$WAIT_FOR" build/serial.log 2>/dev/null; then break; fi
    kill -0 $QPID 2>/dev/null || break
    sleep 0.2
done
sleep 1   # the screen needs a second to finish drawing

python3 - "$SOCK" <<'EOF'
import socket, sys, time
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
time.sleep(0.3); s.recv(4096)
s.send(b"screendump build/screen.ppm\n"); time.sleep(1)
s.send(b"quit\n")
EOF

python3 tools/ppm2png.py build/screen.ppm build/screen.png
echo "---- serial log ----"
cat build/serial.log
