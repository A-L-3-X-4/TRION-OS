#include "kernel.h"
#include "io.h"
#include "irq.h"
#include "arch.h"
#include "abi.h"
#include "task.h"
#include "timer.h"
#include "ipc.h"
#include "obj.h"
#include "ns.h"
#include "sec.h"
#include "vfs.h"
#include "linux.h"
#include "drv.h"
#include "blk.h"
#include "win32.h"

#define MSR_EFER          0xC0000080
#define MSR_STAR          0xC0000081
#define MSR_LSTAR         0xC0000082
#define MSR_SFMASK        0xC0000084
#define MSR_GS_BASE       0xC0000101
#define MSR_KERNEL_GS     0xC0000102
#define EFER_SCE          (1ULL << 0)

#define RFLAGS_TF (1ULL << 8)
#define RFLAGS_DF (1ULL << 10)
#define RFLAGS_AC (1ULL << 18)
#define CR0_EM    (1ULL << 2)

void syscall_init(void)
{
    uint32_t a, b, c, d;
    cpuid(0x80000001, &a, &b, &c, &d);
    if (!(d & (1u << 11)))
        kpanic("cpu has no SYSCALL/SYSRET");

    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
    wrmsr(MSR_STAR, ((uint64_t)SEL_UCODE32 << 48) | ((uint64_t)SEL_KCODE << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, RFLAGS_IF | RFLAGS_DF | RFLAGS_TF | RFLAGS_AC);

    wrmsr(MSR_GS_BASE, (uint64_t)&g_percpu);
    wrmsr(MSR_KERNEL_GS, 0);

    write_cr0(read_cr0() | CR0_EM);
    linux_arch_init();
}

static long sys_write(uint64_t uptr, uint64_t len)
{
    struct task *t = task_current();
    if (!t)
        return -E_INVAL;
    if (len > 4096)
        return -E_INVAL;

    char buf[129];
    uint64_t done = 0;
    while (done < len) {
        size_t n = len - done > 128 ? 128 : (size_t)(len - done);
        int rc = user_copy_in(t, buf, uptr + done, n);
        if (rc)
            return done ? (long)done : rc;
        buf[n] = 0;
        kputs(buf);
        done += n;
    }
    return (long)done;
}

static long sys_msg_send(uint64_t uhdr, uint64_t timeout_ms)
{
    struct task *t = task_current();
    if (!t || !t->ipc)
        return -E_INVAL;
    uint8_t buf[IPC_MSG_MAX];
    struct msg_hdr h;
    int rc = user_copy_in(t, &h, uhdr, sizeof(h));
    if (rc)
        return rc;
    if (h.size < sizeof(h))
        return -E_INVAL;
    if (h.size > IPC_MSG_MAX)
        return -E_MSGSIZE;
    rc = user_copy_in(t, buf, uhdr, h.size);
    if (rc)
        return rc;
    return ipc_send(t->ipc, buf, h.size, timeout_ms);
}

static long sys_msg_recv(uint64_t name, uint64_t ubuf, uint64_t cap, uint64_t timeout_ms)
{
    struct task *t = task_current();
    if (!t || !t->ipc)
        return -E_INVAL;
    if (cap > IPC_MSG_MAX)
        cap = IPC_MSG_MAX;
    int rc = user_check_out(t, ubuf, cap);
    if (rc)
        return rc;
    uint8_t buf[IPC_MSG_MAX];
    long n = ipc_recv(t->ipc, (uint32_t)name, buf, cap, timeout_ms);
    if (n > 0)
        user_copy_out(t, ubuf, buf, (size_t)n);
    return n;
}

static long sys_port_info(uint64_t name, uint64_t uout)
{
    struct task *t = task_current();
    if (!t || !t->ipc)
        return -E_INVAL;
    struct port_info pi;
    int rc = user_check_out(t, uout, sizeof(pi));
    if (rc)
        return rc;
    rc = ipc_port_info(t->ipc, (uint32_t)name, &pi);
    if (rc)
        return rc;
    return user_copy_out(t, uout, &pi, sizeof(pi));
}

static long sys_port(uint64_t nr, uint64_t a, uint64_t b)
{
    struct task *t = task_current();
    if (!t || !t->ipc)
        return -E_INVAL;
    uint32_t name = 0;
    int rc;
    switch (nr) {
    case SYS_PORT_ALLOC:
        rc = ipc_port_alloc(t->ipc, &name);
        return rc ? rc : (long)name;
    case SYS_PORT_DESTROY: return ipc_port_destroy(t->ipc, (uint32_t)a);
    case SYS_PORT_DEALLOC: return ipc_port_dealloc(t->ipc, (uint32_t)a);
    case SYS_PORT_INSERT:  return ipc_port_insert(t->ipc, (uint32_t)a, (uint32_t)b);
    case SYS_PORT_QLIMIT:  return ipc_port_set_qlimit(t->ipc, (uint32_t)a, (uint32_t)b);
    default:               return -E_NOSYS;
    }
}

void syscall_handler(struct regs *r)
{
    long ret;
    struct task *cur = task_current();
    if (cur && cur->lx && cur->lx->flavor == LXF_LINUX) {
        linux_syscall(r);
        return;
    }
    switch (r->rax) {
    case SYS_EXIT:
        thread_exit((int)r->rdi);
    case SYS_WRITE:
        ret = sys_write(r->rdi, r->rsi);
        break;
    case SYS_YIELD:
        thread_yield();
        ret = 0;
        break;
    case SYS_GETTID:
        ret = thread_current()->tid;
        break;
    case SYS_SLEEP_MS:
        thread_sleep_ms(r->rdi);
        ret = 0;
        break;
    case SYS_CLOCK_NS:
        ret = (long)clock_ns();
        break;
    case SYS_GETPID:
        ret = task_current() ? task_current()->id : 0;
        break;
    case SYS_PORT_ALLOC:
    case SYS_PORT_DESTROY:
    case SYS_PORT_DEALLOC:
    case SYS_PORT_INSERT:
    case SYS_PORT_QLIMIT:
        ret = sys_port(r->rax, r->rdi, r->rsi);
        break;
    case SYS_PORT_INFO:
        ret = sys_port_info(r->rdi, r->rsi);
        break;
    case SYS_MSG_SEND:
        ret = sys_msg_send(r->rdi, r->rsi);
        break;
    case SYS_CLOSE: case SYS_DUP: case SYS_EVENT_CREATE: case SYS_EVENT_OP:
    case SYS_WAIT: case SYS_SEM_CREATE: case SYS_SEM_RELEASE:
        ret = sys_obj_dispatch(r->rax, r->rdi, r->rsi);
        break;
    case SYS_NS_MKDIR: case SYS_NS_INSERT: case SYS_NS_OPEN: case SYS_NS_UNLINK:
    case SYS_NS_PUBLISH: case SYS_NS_LOOKUP_PORT: case SYS_NS_QUERY: {
        struct task *t = task_current();
        ret = (t && t->ht && t->ipc) ? sys_ns_dispatch(t, r->rax, r->rdi, r->rsi, r->rdx, r->r10)
                                     : -E_INVAL;
        break;
    }
    case SYS_OBJ_GETSEC: case SYS_OBJ_SETSEC: case SYS_TOKEN_INFO: {
        struct task *t = task_current();
        ret = (t && t->ht) ? sys_sec_dispatch(t, r->rax, r->rdi, r->rsi, r->rdx) : -E_INVAL;
        break;
    }
    case SYS_FILE_OPEN: case SYS_FILE_READ: case SYS_FILE_WRITE: case SYS_FILE_SEEK:
    case SYS_FILE_STAT: case SYS_FILE_SETSIZE: case SYS_FILE_READDIR: case SYS_FILE_FLUSH:
    case SYS_FS_MKDIR: case SYS_FS_REMOVE: case SYS_FS_RENAME: case SYS_FS_STAT: case SYS_FS_STATFS: {
        struct task *t = task_current();
        ret = (t && t->ht) ? sys_fs_dispatch(t, r->rax, r->rdi, r->rsi, r->rdx, r->r10) : -E_INVAL;
        break;
    }
    case SYS_DEV_OPEN: case SYS_DEV_READ: case SYS_DEV_WRITE: case SYS_DEV_IOCTL: case SYS_DEV_SEEK: {
        struct task *t = task_current();
        ret = (t && t->ht) ? sys_dev_dispatch(t, r->rax, r->rdi, r->rsi, r->rdx) : -E_INVAL;
        break;
    }
    case SYS_VM_ALLOC: case SYS_VM_FREE: case SYS_VM_PROTECT: case SYS_VM_QUERY: {
        struct task *t = task_current();
        ret = t ? sys_vm_dispatch(t, r->rax, r->rdi, r->rsi, r->rdx) : -E_INVAL;
        break;
    }
    case SYS_TIME_REAL:
        ret = (long)wall_ns();
        break;
    case SYS_MSG_RECV:
        ret = sys_msg_recv(r->rdi, r->rsi, r->rdx, r->r10);
        break;
    default:
        ret = -E_NOSYS;
        break;
    }
    r->rax = (uint64_t)ret;
}
