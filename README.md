<h1 align="center">TRION OS</h1>
<div align="center">
  <img width="200" height="140" alt="trion-dark" src="https://github.com/user-attachments/assets/1100bdbf-e1ae-4de9-bc21-9ffbf6451df4#gh-dark-mode-only" />
  <img width="200" height="140" alt="trion-light" src="https://github.com/user-attachments/assets/59be1320-06dd-44ad-8350-96e3750554a2#gh-light-mode-only" />
</div>

<br>
An x86_64 kernel written from scratch. It borrows ideas from the three big kernel families and none of their code: NT's object manager and driver stacks, Mach's port IPC, Linux's scheduler and VFS. It boots through GRUB, runs on one CPU, and can run an unmodified static Linux binary or an unmodified MinGW-built Windows `.exe`.
 
It's a hobby kernel. It's developed and tested under QEMU, it's slow in places on purpose and in other places by accident, and the Known limits section below is long. Read it before you get excited.
 
Trion OS is the name. InfinityOS was the internal codename and it's still all over the place (boot banner, GRUB menu entry, the ISO filename, the `uname` string Linux programs see). That's not a bug, we just haven't gotten around to it.
 
## What runs on it
 
- **Linux programs.** Static and static-PIE x86-64 ELF binaries, including a real glibc one (stdio, malloc, qsort, libm, readdir). No `fork`, `execve`, threads, pipes or sockets yet.
- **Windows programs.** x64 PE console executables. A stock `x86_64-w64-mingw32-gcc hello.c` binary runs as is. No threads, no SEH, no DLLs loaded from disk.
- **Native programs.** Flat binaries in ring 3 that talk to the kernel directly through `SYSCALL`.
## How it's put together
 
The core doesn't know what a Linux or Windows program is. Tasks, handles, ports, security tokens, the scheduler and the VFS are all neutral, and the two personalities sit on top.
 
- **Linux** is an ELF loader plus a syscall shim that speaks Linux x86-64 syscall numbers. A Linux binary is just a normal task that also carries some Linux process state. The kernel did not become Linux.
- **Windows** is split the way Windows splits it. The kernel loads PE files and provides native services, including NT-style virtual memory (reserve, commit, protect, query). The Win32 API itself is user-mode code: a runtime with around 300 exports playing ntdll, kernel32 and msvcrt, ending in native syscalls. The kernel has no Win32 syscall table and never runs Win32 code on its own behalf.
What came from where:
 
- **NT:** everything is an object and tasks hold handles with access masks. A namespace with paths like `\Services\echo`. Tokens, SIDs and ACLs using NT's access check. Driver stacks with IRPs, completion routines and cancellation.
- **Mach:** ports and rights, messages that carry rights as descriptors, send-once rights.
- **Linux:** an EEVDF-style scheduler (nice becomes weight, eligibility, earliest virtual deadline), the VFS, and the syscall ABI the Linux shim speaks.
The rest of the basics: Multiboot2 boot, 32-bit to long mode, a 4 GiB identity map, a 1000 Hz PIT tick with a TSC-backed clock, kernel threads with real context switches, per-task address spaces, and `SYSCALL`/`SYSRET` for user mode. Storage is polled ATA PIO, MBR partitions, RAM disks and a FAT32 driver with long names and in-kernel `mkfs`.
 
## The Windows part was a pain in the ass
 
Windows binaries were by far the worst part of this project. Stuff broke constantly, and getting the binaries we use to test the w32 syscalls running on Windows took a pile of dumb workarounds, like putting the entire OS into VGA mode. Linux was easy. So was everything else.
 
One thing that came out of it: Windows paths. `C:\example\dir`, `C:/example/dir` and `\example\dir` all end up as `/Volumes/<first volume>/example/dir`, in both personalities. `C:` is the first volume under `\Volumes`, `D:` is the second, and so on, so your drive letters are decided by mount order. There's no `A:` or `B:`, and the case of the path is left alone.
 
`w32min.exe`, the freestanding Windows test program, also runs on real Windows. It tells which one it's on by asking kernel32 for an export only we have (`InfinityOsVersion`; real Windows has never heard of it) and skips the checks that pin Trion-only behavior when it finds out it's on the real thing. That's how we check that we behave like Windows and not like what we assume Windows does.
 
## Build
 
You need gcc, nasm, ld, make, `grub-mkrescue` (with xorriso and mtools), `qemu-system-x86_64`, static glibc for the Linux test programs (`libc6-dev`: `libc.a` and `rcrt1.o`), and `gcc-mingw-w64-x86-64` for the Windows test programs. The test disks also need `mkfs.fat`, mtools and python3.
 
On Debian or Ubuntu that's roughly:
 
```
sudo apt install build-essential nasm grub-pc-bin xorriso mtools dosfstools \
    qemu-system-x86 libc6-dev gcc-mingw-w64-x86-64 python3
```
 
```
make                        # build/kernel.elf and the ISO
make run                    # boot it in QEMU, serial output on your terminal
make test                   # headless boot, runs every suite, fscks the disk images
make check                  # confirms the kernel is a valid multiboot2 image
make clean
make TEST_FAULT=1           # page-faults on purpose at the end, to test the handler
make TEST_STACKOVERFLOW=1   # a thread recurses into its guard page
```
 
There's a MinGW override if yours has a different prefix: `make MINGW=your-prefix`.
 
## Tests
 
`make test` boots the ISO headless with four test disk images attached, waits for the last suite to report, then runs `fsck.fat` and reads every written file back with mtools. It writes `build/serial.log` and a screenshot to `build/screen.png`. A full run takes about two minutes.
 
The suites print `PASS` or `FAIL` plus a check count at boot: `USERTEST`, `IPCTEST`, `OBJTEST`, `NSTEST`, `SECTEST`, `FSTEST`, `LXTEST`, `DRVTEST` and `W32TEST`. The counts change every time something gets added, so look at your own log instead of trusting a number in a README.
 
For a fast edit and test loop on the Windows side:
 
```
make ONLY_W32=1 iso    # boots straight into the Win32 suite and skips everything else
```
 
Touch `kernel/schedtest.c` when you switch between that and a normal build.
 
The filesystem tests only write to disk images that carry the `KERNEL.TST` marker file, or to a scratch disk tagged `INFINITYOS-FSTEST-SCRATCH`.
 
## Layout
 
```
boot/             multiboot2 header, long mode switch, grub.cfg
kernel/           the kernel
  mm/             physical frames, virtual memory, heap, preemption guard
  include/        headers, abi.h is the contract shared with user code
  lib/            string functions
user/             programs the boot tests run
  linux/          Linux test programs
  w32/            the Win32 runtime and the Windows test programs
tools/            test runner, disk image builder, fsck check, font generator
linker.ld         kernel is loaded at 1 MiB
```
 
Where things live inside `kernel/`:
 
```
main.c gdt.c idt.c isr.asm irq.c timer.c       boot, CPU setup, interrupts
sched.c switch.asm                              scheduler, threads, semaphores
task.c syscall.c syscall_entry.S                tasks, user copies, SYSCALL path
ipc.c                                           Mach-style ports
obj.c objsync.c ns.c sec.c                      object manager, namespace, ACLs
blk.c ata.c rtc.c vfs.c fat32.c fssys.c         storage, FAT32, VFS
elf.c linux.c                                   ELF loader, Linux syscall shim
pe.c win32.c winvm.c                            PE loader, Win32 processes, NT VM
drv.c drvsys.c drivers.c                        driver framework, built-in drivers
console.c printf.c panic.c                      framebuffer and serial console, crash screen
*test.c                                         the boot-time suites
```
 
The console asks GRUB for a 640x480x32 framebuffer and draws an 80-column CP437 grid with an 8x16 font (`font8x16.c`, generated by `tools/genfont.py` from GNU Unifont). If GRUB can't provide a 32-bit framebuffer below 4 GiB it falls back to the VGA text buffer. Serial output works either way.
 
## Known limits
 
Hardware and core:
- One CPU, no SMP. PIC, not APIC. RAM above 4 GiB is ignored.
- No keyboard driver. stdin is always at EOF in both personalities.
- No FPU state for native tasks (it's disabled so they fault instead of leaking state). Linux and Windows tasks get real x87 and SSE. No AVX.
- Interrupt handlers can't block or allocate. No SMAP or SMEP.
Storage:
- FAT32 only. No FAT12, FAT16, exFAT or GPT.
- All disk I/O is polled PIO, which is slow on real hardware.
- ATA LBA48 above 2^28 sectors and real 4Kn disks are untested. QEMU can't emulate them.
Linux personality:
- No `fork`, `clone`, `execve`, `wait4`, threads, pipes, sockets or `poll`.
- Signals aren't delivered. A fault kills the task with `-(100 + vector)`.
- `mremap` fails (libc falls back to copying), and `getrandom` is an xorshift, not cryptographic.
Windows personality:
- One thread per process. `CreateThread` fails, so critical sections never block.
- No structured exception handling. A fault kills the process.
- `LoadLibrary` only knows the built-in DLL names. No PE DLLs from disk.
- No overlapped I/O, no `WaitForMultipleObjects`, no named events or semaphores.
- stdio is unbuffered, there's no `scanf`, and `%e` and `%g` print like `%f`.
- No 32-bit images, no time zones (local time is UTC), no console input.
Drivers are compiled into the kernel. There's no PnP, no power management and no driver image loader.
 
## Where it's going
 
Roughly a ten step plan. Boot and memory, the scheduler, user mode and ports, the object manager, FAT32 and the VFS, the ELF loader and Linux shim, the driver framework, and the PE loader and Win32 personality are done. Step 7 is a milestone, running a real static Linux program end to end, and it's mostly met: what's left is process creation, pipes, signals, threads and real console input. Step 10 is a Mach-O and macOS personality, saved for last because it has the least documentation. The Win32 design (loader in the kernel, API layer in use
