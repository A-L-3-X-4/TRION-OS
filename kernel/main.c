#include "kernel.h"
#include "drv.h"
#include "ns.h"
#include "io.h"
#include "mm.h"
#include "multiboot2.h"
#include "irq.h"
#include "timer.h"
#include "sched.h"
#include "arch.h"
#include "blk.h"
#include "vfs.h"

static const char *mem_type_name(uint32_t t)
{
    switch (t) {
    case MB2_MEM_AVAILABLE: return "usable";
    case MB2_MEM_RESERVED:  return "reserved";
    case MB2_MEM_ACPI:      return "ACPI reclaimable";
    case MB2_MEM_NVS:       return "ACPI NVS";
    case MB2_MEM_BAD:       return "bad";
    default:                return "unknown";
    }
}

void kmain(uint64_t mbi_addr, uint64_t magic)
{
    serial_init();
    console_init();
    if (magic == MB2_BOOT_MAGIC)
        console_fb_init(mbi_addr);

    console_set_color(C_LCYAN, C_BLACK);
    console_vga_puts("\xEC\xEC InfinityOS " INFINITY_VERSION " \xEC\xEC\n");
    serial_puts("== InfinityOS " INFINITY_VERSION " ==\r\n");
    console_set_color(C_LGRAY, C_BLACK);

    if (magic != MB2_BOOT_MAGIC)
        kpanic("not booted by a Multiboot2 loader (magic %lx)", magic);

    const struct mb2_tag *bl = mb2_find_tag(mbi_addr, MB2_TAG_BOOTLOADER);
    if (bl)
        klog(LOG_INFO, "bootloader: %s", (const char *)bl + 8);
    klog(LOG_INFO, "console: %s", console_fb_active() ? "graphical framebuffer (8x16 glyph grid)" : "VGA text mode");

    gdt_init();
    idt_init();
    klog(LOG_OK, "gdt+tss: loaded (IST1 = double-fault stack); idt: exceptions 0-31, IRQs 32-47");

    const struct mb2_tag_mmap *mm =
        (const struct mb2_tag_mmap *)mb2_find_tag(mbi_addr, MB2_TAG_MMAP);
    if (mm) {
        klog(LOG_INFO, "memory map:");
        MB2_FOR_EACH_MMAP(mm, e)
            kprintf("         %010lx - %010lx  %s\n",
                    e->addr, e->addr + e->len - 1, mem_type_name(e->type));
    }

    pmm_init(mbi_addr);
    klog(LOG_OK, "pmm: %lu MiB free (address span %lu MiB, only <4 GiB used for now)",
         (uint64_t)(pmm_free_frames() * PAGE_SIZE / MiB),
         (uint64_t)(pmm_total_frames() * PAGE_SIZE / MiB));

    vmm_init();
    klog(LOG_OK, "vmm: paging up, 4 GiB identity map, NX %s",
         vmm_nx_enabled() ? "enabled" : "unavailable");

    heap_init();
    klog(LOG_OK, "heap: online at %p", (void *)KERNEL_HEAP_BASE);

    syscall_init();
    klog(LOG_OK, "syscall: SYSCALL/SYSRET armed (STAR/LSTAR/SFMASK), kernel GS = percpu, CR0.EM set");

    unsigned failed = selftest_run();

    console_set_color(failed ? C_LRED : C_LGREEN, C_BLACK);
    kprintf("\nStep 1 (boot + memory) %s.\n\n", failed ? "has FAILURES" : "complete");
    console_set_color(C_LGRAY, C_BLACK);

    pic_init();
    timer_init(1000);
    klog(LOG_OK, "pic: remapped to 32-47; pit: %u Hz, clock %s (%lu MHz)", timer_hz(),
         timer_tsc_calibrated() ? "TSC-backed" : "tick-based",
         timer_tsc_khz() / 1000);

    sched_init();
    if (ns_init())
        kpanic("cannot create the object namespace");
    klog(LOG_OK, "sched: EEVDF online, idle + reaper threads up");

    rtc_init();
    vfs_init();
    int ndisks = ata_init();
    for (unsigned i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_nth(i);
        int np = d ? blk_scan_partitions(d) : 0;
        if (np)
            klog(LOG_INFO, "blk: %s has %d partition(s)", d->name, np);
    }
    int nvol = vfs_automount();
    io_init();
    drivers_init();
    blkdisk_scan();
    klog(LOG_OK, "storage: %d disk(s), %d volume(s) mounted under \\Volumes", ndisks, nvol);

    // The test suite is just a kernel thread so it can block, sleep and get preempted like everything else
    struct thread *st = thread_create("schedtest", schedtest_main, NULL, 0);
    if (!st)
        kpanic("cannot start schedtest thread");
    thread_detach(st);

    // Interrupts stay off until every subsystem is up, now the timer can tick and the scheduler gets to take over
    irq_enable();
    // Nothing left to do, so this becomes the idle loop and sits in sti; hlt until something needs scheduling
    sched_idle_loop();
}
