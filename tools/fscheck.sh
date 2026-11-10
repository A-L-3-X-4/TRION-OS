#!/usr/bin/env bash

set -uo pipefail
cd "$(dirname "$0")/.."
export MTOOLS_SKIP_CHECK=1
fail=0

for img in build/disk.img build/scratch.img build/disk4k.img build/scratch2k.img; do
    [ -f "$img" ] || continue
    echo "== fsck.fat $img"
    if out=$(fsck.fat -n -v "$img" 2>&1); then
        echo "$out" | grep -E "bytes per logical sector|clusters|files|bytes in" | sed 's/^/   /'
        echo "   clean"
    else
        echo "$out" | sed 's/^/   /'
        echo "   FSCK FAILED"
        fail=1
    fi
done

python3 - <<'PY' || fail=1
import subprocess, sys
def pat(n, seed=0):
    return bytes((((i+seed) ^ ((i+seed) >> 8) ^ ((i+seed) >> 16)) * 167 + 13) & 0xFF for i in range(n))
def get(img, path):
    r = subprocess.run(['mcopy', '-n', '-i', img, '::' + path, '-'], capture_output=True)
    return r.stdout if r.returncode == 0 else None
def ls(img, path=''):
    r = subprocess.run(['mdir', '-b', '-i', img, '::' + path], capture_output=True, text=True)
    return [l.rstrip('/').split('/')[-1] for l in r.stdout.splitlines() if l] if r.returncode == 0 else None
bad = 0
def check(ok, what):
    global bad
    print(('   ok   ' if ok else '   FAIL ') + what)
    if not ok: bad += 1

print('== disk.img: files written by the kernel into an mkfs.fat volume')
D = 'build/disk.img'
check(get(D, 'HELLO.TXT') == b'hello from the host\n + kernel\n', 'HELLO.TXT appended to')
check(get(D, 'KERNEL/OUT.BIN') == pat(2*1024*1024 + 4321, 5), 'KERNEL/OUT.BIN 2 MiB pattern')
check(get(D, 'KERNEL/Sub Dir/naïve café.txt') == b'unicode name\n' or
      any(b'unicode name' in (get(D, 'KERNEL/Sub Dir/' + f) or b'') for f in (ls(D, 'KERNEL/Sub Dir') or [])),
      'unicode long name readable (UTF-16 LFN written by the kernel)')
check(get(D, 'KERNEL/Lower Case.txt') == b'mixed case\n', 'KERNEL/Lower Case.txt (LFN)')
check(get(D, 'KERNEL/short.txt') == b'all lower 8.3\n', 'KERNEL/short.txt (NT lower-case flag, no LFN)')
check(get(D, 'KERNEL/UPPER.TXT') == b'all upper 8.3\n', 'KERNEL/UPPER.TXT')
big = bytearray(pat(3*1024*1024 + 777)); big[1000000:1005000] = b'\xaa' * 5000
check(get(D, 'BIG.BIN') == bytes(big), 'BIG.BIN: 3 MiB, middle 5000 bytes overwritten')
check(get(D, 'Renamed Lower.txt') == b'lower\n' and get(D, 'lower.txt') is None, 'lower.txt renamed (LFN created)')
check(get(D, 'Long File Name Example.txt') is None, 'deleted file is gone')
check(get(D, 'DOCS/moved deep.txt') == b'deep\n' and ls(D, 'DOCS/NESTED') is None, 'file moved across dirs, empty dir removed')
check(get(D, 'LOCKED.TXT') == b'read only\n', 'read-only file untouched')
check(get(D, 'KERNEL.TST') is None, 'marker removed (writes happen once)')
names = ls(D, 'KERNEL') or []
check(sorted(n.lower() for n in names) == sorted(['out.bin', 'sub dir', 'lower case.txt', 'short.txt', 'upper.txt']),
      'KERNEL directory listing: %s' % names)

print('== scratch.img: kernel mkfs + workload, checked against its own manifest')
S = 'build/scratch.img'
man = get(S, 'MANIFEST.TXT')
check(man is not None, 'MANIFEST.TXT present')
if man:
    lines = [l for l in man.decode().split('\n') if l]
    ok = 0
    for l in lines:
        path, size, seed = l.rsplit(' ', 2)
        data = get(S, path.replace('\\', '/'))
        if data is not None and data == pat(int(size), int(seed)):
            ok += 1
        else:
            print('   FAIL', path, size, seed, None if data is None else len(data))
            bad += 1
    check(ok == len(lines), '%d/%d manifest files match their pattern' % (ok, len(lines)))
    check(get(S, 'alpha/file-00.dat') is None and get(S, 'beta gamma/FILE04.dat') is None, 'deleted files are gone')
    check(ls(S, 'alpha/deeper') is None and ls(S, 'beta gamma/deeper') is not None, 'moved directory')

print('== disk4k.img: 4096-byte sectors, files written by the kernel into an mkfs.fat -S 4096 volume')
K = 'build/disk4k.img'
check(get(K, 'HELLO.TXT') == b'hello from a 4096-byte-sector volume\n + kernel\n', 'HELLO.TXT appended to')
check(get(K, 'KERNEL/OUT.BIN') == pat(1024*1024 + 4321, 5), 'KERNEL/OUT.BIN 1 MiB pattern')
check(get(K, 'KERNEL/Lower Case.txt') == b'mixed case\n' and get(K, 'KERNEL/UPPER.TXT') == b'all upper 8.3\n', 'KERNEL files (LFN + 8.3)')
big = bytearray(pat(3*1024*1024 + 777)); big[4090:4090+9000] = b'\xaa' * 9000
check(get(K, 'BIG.BIN') == bytes(big), 'BIG.BIN: 9000 bytes overwritten across 4 KiB sector boundaries')
check(get(K, 'Long File Name Example.txt') is None, 'deleted file is gone')
check(get(K, 'DOCS/moved deep.txt') == b'deep\n' and ls(K, 'DOCS/NESTED') is None, 'file moved, empty dir removed')
check(get(K, 'KERNEL.TST') is None, 'marker removed (writes happen once)')
many = ls(K, 'KERNEL/MANY') or []
want = ['Many Files With Long Names %02d.txt' % i for i in range(70) if i != 13]
check(sorted(many) == sorted(want), 'KERNEL/MANY: 69 long-name files in a directory spanning two clusters (%d listed)' % len(many))
okc = sum(1 for i in range(70) if i != 13 and get(K, 'KERNEL/MANY/Many Files With Long Names %02d.txt' % i) == ('file #%02d\n' % i).encode())
check(okc == 69, '%d/69 MANY files have the right content' % okc)

print('== scratch2k.img: kernel mkfs with 2048-byte sectors + workload, checked against its own manifest')
S = 'build/scratch2k.img'
man = get(S, 'MANIFEST.TXT')
check(man is not None, 'MANIFEST.TXT present')
if man:
    lines = [l for l in man.decode().split('\n') if l]
    ok = 0
    for l in lines:
        path, size, seed = l.rsplit(' ', 2)
        data = get(S, path.replace('\\', '/'))
        if data is not None and data == pat(int(size), int(seed)):
            ok += 1
        else:
            print('   FAIL', path, size, seed, None if data is None else len(data))
            bad += 1
    check(ok == len(lines), '%d/%d manifest files match their pattern' % (ok, len(lines)))
    check(get(S, 'alpha/file-00.dat') is None and get(S, 'beta gamma/FILE04.dat') is None, 'deleted files are gone')
    check(ls(S, 'alpha/deeper') is None and ls(S, 'beta gamma/deeper') is not None, 'moved directory')
sys.exit(1 if bad else 0)
PY

[ $fail -eq 0 ] && echo "AUDIT PASS" || { echo "AUDIT FAIL"; exit 1; }
