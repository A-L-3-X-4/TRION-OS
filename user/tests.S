#include "abi.h"

    .intel_syntax noprefix
    .text
    .globl ut_cpl
    .globl ut_hello

    .org UT_CPL
ut_cpl:
    mov     ax, cs
    and     eax, 3
    mov     edi, eax
    mov     eax, SYS_EXIT
    syscall
    ud2

    .org UT_HELLO
ut_hello:
    mov     eax, SYS_WRITE
    lea     rdi, [rip + hello_msg]
    mov     esi, offset hello_len
    syscall
    mov     edi, 1
    cmp     rax, offset hello_len
    jne     1f
    mov     edi, 42
1:  mov     eax, SYS_EXIT
    syscall
    ud2
hello_msg:
    .ascii  "[user] hello from ring 3\n"
    .set    hello_len, . - hello_msg

    .macro  SET reg, val
    movabs  \reg, \val
    .endm
    .macro  CHK reg, val
    movabs  rax, \val
    cmp     \reg, rax
    jne     regs_fail
    .endm

    .org UT_REGS
ut_regs:
    SET rbx, 0x1111111111111111
    SET rbp, 0x2222222222222222
    SET rsi, 0x3333333333333333
    SET rdi, 0x4444444444444444
    SET rdx, 0x5555555555555555
    SET r8,  0x6666666666666666
    SET r9,  0x7777777777777777
    SET r10, 0x8888888888888888
    SET r12, 0x9999999999999999
    SET r13, 0xAAAAAAAAAAAAAAAA
    SET r14, 0xBBBBBBBBBBBBBBBB
    SET r15, 0xCCCCCCCCCCCCCCCC
    mov     eax, SYS_YIELD
    syscall
    CHK rbx, 0x1111111111111111
    CHK rbp, 0x2222222222222222
    CHK rsi, 0x3333333333333333
    CHK rdi, 0x4444444444444444
    CHK rdx, 0x5555555555555555
    CHK r8,  0x6666666666666666
    CHK r9,  0x7777777777777777
    CHK r10, 0x8888888888888888
    CHK r12, 0x9999999999999999
    CHK r13, 0xAAAAAAAAAAAAAAAA
    CHK r14, 0xBBBBBBBBBBBBBBBB
    CHK r15, 0xCCCCCCCCCCCCCCCC
    xor     edi, edi
    mov     eax, SYS_EXIT
    syscall
regs_fail:
    mov     edi, 1
    mov     eax, SYS_EXIT
    syscall
    ud2

    .org UT_BADSYS
ut_badsys:
    mov     eax, 999
    syscall
    cmp     rax, -E_NOSYS
    jne     bad
    mov     eax, SYS_WRITE
    mov     rdi, 0x100000
    mov     esi, 8
    syscall
    cmp     rax, -E_FAULT
    jne     bad
    mov     eax, SYS_WRITE
    mov     rdi, USER_BASE + 0x50000
    mov     esi, 8
    syscall
    cmp     rax, -E_FAULT
    jne     bad
    mov     eax, SYS_WRITE
    lea     rdi, [rip + ut_badsys]
    mov     esi, 100000
    syscall
    cmp     rax, -E_INVAL
    jne     bad
    xor     edi, edi
    mov     eax, SYS_EXIT
    syscall
bad:
    mov     edi, 1
    mov     eax, SYS_EXIT
    syscall
    ud2

    .org UT_FAULT_W
ut_fault_w:
    lea     rax, [rip + ut_fault_w]
    mov     byte ptr [rax], 0x90
    jmp     survived

    .org UT_FAULT_K
ut_fault_k:
    mov     rax, 0x100000
    mov     rax, [rax]
    jmp     survived

    .org UT_FAULT_PRIV
ut_fault_priv:
    cli
    jmp     survived

survived:
    mov     edi, 99
    mov     eax, SYS_EXIT
    syscall
    ud2

    .org UT_ISO
ut_iso:
    push    rdi
    mov     r12d, 20
1:  mov     eax, SYS_YIELD
    syscall
    dec     r12d
    jnz     1b
    pop     rdi
    mov     eax, SYS_EXIT
    syscall
    ud2

    .org UT_SPIN
ut_spin:
    mov     eax, SYS_CLOCK_NS
    syscall
    mov     r12, rax
1:  mov     r13d, 200000
2:  dec     r13d
    jnz     2b
    mov     eax, SYS_CLOCK_NS
    syscall
    sub     rax, r12
    cmp     rax, 150000000
    jb      1b
    xor     edi, edi
    mov     eax, SYS_EXIT
    syscall
    ud2

    .org UT_FPU
ut_fpu:
    pxor    xmm0, xmm0
    jmp     survived

    .macro  SC nr
    mov     eax, \nr
    syscall
    .endm

    .macro  HDR base, bits, size, remote, local, id, ndesc
    mov     dword ptr [\base + 0],  \bits
    mov     dword ptr [\base + 4],  \size
    mov     dword ptr [\base + 8],  \remote
    mov     dword ptr [\base + 12], \local
    mov     dword ptr [\base + 16], \id
    mov     dword ptr [\base + 20], \ndesc
    .endm

    .macro  EXPECT val, code
    cmp     rax, \val
    je      9f
    mov     edi, \code
    jmp     ipc_exit
9:
    .endm

    .macro  EXPECT32 off, val, code
    cmp     dword ptr [rbp + \off], \val
    je      9f
    mov     edi, \code
    jmp     ipc_exit
9:
    .endm

    .macro  EXPECT64 off, val, code
    movabs  rax, \val
    cmp     qword ptr [rbp + \off], rax
    je      9f
    mov     edi, \code
    jmp     ipc_exit
9:
    .endm

    .macro  RECV name, off, cap, tmo
    mov     edi, \name
    lea     rsi, [rbp + \off]
    mov     edx, \cap
    mov     r10, \tmo
    SC      SYS_MSG_RECV
    .endm

    .macro  SEND_SELF
    HDR     rbp,MSG_BITS(MSG_COPY_SEND,0),32,r12d,0,0x77,0
    movabs  rax, 0x1122334455667788
    mov     [rbp + 24], rax
    mov     rdi, rbp
    xor     esi, esi
    SC      SYS_MSG_SEND
    .endm

ipc_exit:
    mov     eax, SYS_EXIT
    syscall
    ud2

    .org UT_PORT_BASIC
ut_port_basic:
    sub     rsp, 1024
    mov     rbp, rsp

    SC      SYS_PORT_ALLOC
    test    rax, rax
    jg      1f
    mov     edi, 0x10
    jmp     ipc_exit
1:  mov     r12, rax

    mov     edi, r12d
    mov     esi, MSG_MAKE_SEND
    SC      SYS_PORT_INSERT
    EXPECT  0, 0x11

    SEND_SELF
    EXPECT  0, 0x12

    mov     edi, r12d
    lea     rsi, [rbp + 512]
    SC      SYS_PORT_INFO
    EXPECT  0, 0x13
    EXPECT32 512, PT_RECV+PT_SEND, 0x14
    EXPECT32 516, 1, 0x15
    EXPECT32 520, 1, 0x16
    EXPECT32 524, 16, 0x17

    RECV    r12d, 128, 256, 0
    EXPECT  32, 0x18
    EXPECT32 128+0, 0, 0x19
    EXPECT32 128+4, 32, 0x1A
    EXPECT32 128+8, 0, 0x1B
    EXPECT32 128+12, r12d, 0x1C
    EXPECT32 128+16, 0x77, 0x1D
    EXPECT64 128+24, 0x1122334455667788, 0x1E

    RECV    r12d, 128, 256, 0
    EXPECT  -E_AGAIN, 0x20

    SC      SYS_CLOCK_NS
    mov     r13, rax
    RECV    r12d, 128, 256, 30
    EXPECT  -E_TIMEDOUT, 0x21
    SC      SYS_CLOCK_NS
    sub     rax, r13
    cmp     rax, 25000000
    jae     2f
    mov     edi, 0x22
    jmp     ipc_exit
2:
    mov     edi, r12d
    mov     rsi, 0x100000
    mov     edx, 256
    xor     r10d, r10d
    SC      SYS_MSG_RECV
    EXPECT  -E_FAULT, 0x23
    mov     edi, r12d
    lea     rsi, [rip + ut_port_basic]
    mov     edx, 256
    xor     r10d, r10d
    SC      SYS_MSG_RECV
    EXPECT  -E_FAULT, 0x24
    mov     rdi, USER_BASE + 0x50000
    xor     esi, esi
    SC      SYS_MSG_SEND
    EXPECT  -E_FAULT, 0x25
    mov     edi, r12d
    mov     rsi, USER_BASE + 0x50000
    SC      SYS_PORT_INFO
    EXPECT  -E_FAULT, 0x26

    SEND_SELF
    EXPECT  0, 0x27
    RECV    r12d, 128, 31, 0
    EXPECT  -E_MSGSIZE, 0x28
    RECV    r12d, 128, 256, 0
    EXPECT  32, 0x29

    HDR     rbp,MSG_BITS(MSG_COPY_SEND,0),33,r12d,0,0x77,0
    mov     rdi, rbp
    xor     esi, esi
    SC      SYS_MSG_SEND
    cmp     rax, 0
    je      3f
    mov     edi, 0x2A
    jmp     ipc_exit
3:  RECV    r12d, 128, 256, 0
    EXPECT  33, 0x2B
    HDR     rbp,MSG_BITS(MSG_COPY_SEND,0),20,r12d,0,0x77,0
    mov     rdi, rbp
    xor     esi, esi
    SC      SYS_MSG_SEND
    EXPECT  -E_INVAL, 0x2C
    HDR     rbp,MSG_BITS(99,0),32,r12d,0,0x77,0
    mov     rdi, rbp
    xor     esi, esi
    SC      SYS_MSG_SEND
    EXPECT  -E_INVAL, 0x2D
    HDR     rbp,MSG_BITS(MSG_COPY_SEND,0),32,0x1234,0,0x77,0
    mov     rdi, rbp
    xor     esi, esi
    SC      SYS_MSG_SEND
    EXPECT  -E_BADF, 0x2E
    HDR     rbp,MSG_BITS(MSG_COPY_SEND,0),32,r12d,0,0x77,9
    mov     rdi, rbp
    xor     esi, esi
    SC      SYS_MSG_SEND
    EXPECT  -E_INVAL, 0x2F

    mov     edi, 0x1234
    SC      SYS_PORT_DEALLOC
    EXPECT  -E_BADF, 0x30
    mov     edi, r12d
    SC      SYS_PORT_DEALLOC
    EXPECT  0, 0x31
    mov     edi, r12d
    SC      SYS_PORT_DEALLOC
    EXPECT  -E_PERM, 0x32
    SEND_SELF
    EXPECT  -E_PERM, 0x33

    mov     edi, r12d
    mov     esi, 4
    SC      SYS_PORT_QLIMIT
    EXPECT  0, 0x34
    mov     edi, r12d
    mov     esi, 0
    SC      SYS_PORT_QLIMIT
    EXPECT  -E_INVAL, 0x35

    mov     edi, r12d
    SC      SYS_PORT_DESTROY
    EXPECT  0, 0x36
    mov     edi, r12d
    SC      SYS_PORT_DESTROY
    EXPECT  -E_BADF, 0x37

    xor     edi, edi
    jmp     ipc_exit

    .org UT_RPC_SERVER
ut_rpc_server:
    mov     r12d, edi
    sub     rsp, 1024
    mov     rbp, rsp

    RECV    r12d, 0, 512, -1
    EXPECT  32, 0x41
    EXPECT32 0, MSG_BITS(0,MSG_MOVE_SEND_ONCE), 0x42
    EXPECT32 4, 32, 0x43
    EXPECT32 12, r12d, 0x44
    EXPECT32 16, 7, 0x45
    EXPECT64 24, 0x0102030405060708, 0x46
    mov     r13d, dword ptr [rbp + 8]
    test    r13d, r13d
    jnz     1f
    mov     edi, 0x47
    jmp     ipc_exit
1:
    mov     edi, r13d
    lea     rsi, [rbp + 512]
    SC      SYS_PORT_INFO
    EXPECT  0, 0x48
    EXPECT32 512, PT_SONCE, 0x49

    HDR     rbp+256,MSG_BITS(MSG_MOVE_SEND_ONCE,0),32,r13d,0,8,0
    mov     rax, [rbp + 24]
    inc     rax
    mov     [rbp + 256 + 24], rax
    lea     rdi, [rbp + 256]
    mov     rsi, -1
    SC      SYS_MSG_SEND
    EXPECT  0, 0x4A

    mov     edi, r13d
    lea     rsi, [rbp + 512]
    SC      SYS_PORT_INFO
    EXPECT  -E_BADF, 0x4B
    xor     edi, edi
    jmp     ipc_exit

    .org UT_RPC_CLIENT
ut_rpc_client:
    mov     r12d, edi
    sub     rsp, 1024
    mov     rbp, rsp

    SC      SYS_PORT_ALLOC
    test    rax, rax
    jg      1f
    mov     edi, 0x51
    jmp     ipc_exit
1:  mov     r13, rax

    HDR     rbp,MSG_BITS(MSG_COPY_SEND,MSG_MAKE_SEND_ONCE),32,r12d,r13d,7,0
    movabs  rax, 0x0102030405060708
    mov     [rbp + 24], rax
    mov     rdi, rbp
    mov     rsi, -1
    SC      SYS_MSG_SEND
    EXPECT  0, 0x52

    RECV    r13d, 256, 512, 5000
    EXPECT  32, 0x53
    EXPECT32 256+0, 0, 0x54
    EXPECT32 256+16, 8, 0x55
    EXPECT64 256+24, 0x0102030405060709, 0x56
    xor     edi, edi
    jmp     ipc_exit

    .macro  HC nr, a, b
    mov     rdi, \a
    mov     rsi, \b
    SC      \nr
    .endm

    .org UT_OBJ_BASIC
ut_obj_basic:
    HC      SYS_EVENT_CREATE, 0, 0
    test    rax, rax
    jg      1f
    mov     edi, 0x10
    jmp     ipc_exit
1:  mov     r12, rax

    HC      SYS_WAIT, r12, 0
    EXPECT  -E_AGAIN, 0x11
    HC      SYS_EVENT_OP, r12, EVENT_SET
    EXPECT  0, 0x12
    HC      SYS_WAIT, r12, 0
    EXPECT  0, 0x13
    HC      SYS_WAIT, r12, 0
    EXPECT  -E_AGAIN, 0x14

    SC      SYS_CLOCK_NS
    mov     r13, rax
    HC      SYS_WAIT, r12, 20
    EXPECT  -E_TIMEDOUT, 0x15
    SC      SYS_CLOCK_NS
    sub     rax, r13
    cmp     rax, 15000000
    jae     2f
    mov     edi, 0x16
    jmp     ipc_exit
2:
    HC      SYS_EVENT_CREATE, 3, 0
    mov     r14, rax
    HC      SYS_WAIT, r14, 0
    EXPECT  0, 0x17
    HC      SYS_WAIT, r14, 0
    EXPECT  0, 0x18
    HC      SYS_EVENT_OP, r14, EVENT_RESET
    EXPECT  1, 0x19
    HC      SYS_WAIT, r14, 0
    EXPECT  -E_AGAIN, 0x1A

    HC      SYS_DUP, r12, ACCESS_SYNCHRONIZE
    test    rax, rax
    jg      3f
    mov     edi, 0x1B
    jmp     ipc_exit
3:  mov     r15, rax
    HC      SYS_EVENT_OP, r15, EVENT_SET
    EXPECT  -E_ACCES, 0x1C
    HC      SYS_WAIT, r15, 0
    EXPECT  -E_AGAIN, 0x1D
    HC      SYS_DUP, r15, EVENT_MODIFY_STATE
    EXPECT  -E_ACCES, 0x1E

    HC      SYS_DUP, r12, 0
    mov     rbx, rax
    HC      SYS_EVENT_OP, rbx, EVENT_SET
    EXPECT  0, 0x20
    HC      SYS_CLOSE, r12, 0
    EXPECT  0, 0x21
    HC      SYS_CLOSE, r12, 0
    EXPECT  -E_BADF, 0x22
    HC      SYS_WAIT, r12, 0
    EXPECT  -E_BADF, 0x23
    HC      SYS_WAIT, rbx, 0
    EXPECT  0, 0x24

    HC      SYS_SEM_CREATE, 2, 3
    mov     r12, rax
    HC      SYS_WAIT, r12, 0
    EXPECT  0, 0x30
    HC      SYS_WAIT, r12, 0
    EXPECT  0, 0x31
    HC      SYS_WAIT, r12, 0
    EXPECT  -E_AGAIN, 0x32
    HC      SYS_SEM_RELEASE, r12, 1
    EXPECT  0, 0x33
    HC      SYS_WAIT, r12, 0
    EXPECT  0, 0x34
    HC      SYS_SEM_RELEASE, r12, 4
    EXPECT  -E_INVAL, 0x35
    HC      SYS_SEM_RELEASE, r12, 3
    EXPECT  0, 0x36
    HC      SYS_SEM_RELEASE, r12, 1
    EXPECT  -E_INVAL, 0x37

    HC      SYS_EVENT_OP, r12, EVENT_SET
    EXPECT  -E_INVAL, 0x38
    HC      SYS_SEM_CREATE, 0, 0
    EXPECT  -E_INVAL, 0x39
    HC      SYS_CLOSE, 0x1234, 0
    EXPECT  -E_BADF, 0x3A
    HC      SYS_WAIT, 0, 0
    EXPECT  -E_BADF, 0x3B
    xor     edi, edi
    jmp     ipc_exit

    .org UT_OBJ_WAITER
ut_obj_waiter:
    mov     r12, rdi
    HC      SYS_WAIT, r12, 5000
    EXPECT  0, 0x41
    xor     edi, edi
    jmp     ipc_exit

    .macro  NSP nr, str, len, a=0, b=0
    lea     rdi, [rip + \str]
    mov     esi, \len
    mov     rdx, \a
    mov     r10, \b
    SC      \nr
    .endm

    .org UT_NS_BASIC
ut_ns_basic:
    sub     rsp, 1024
    mov     rbp, rsp

    NSP     SYS_NS_MKDIR, s_test, 5
    EXPECT  0, 0x10
    NSP     SYS_NS_MKDIR, s_test, 5
    EXPECT  -E_EXIST, 0x11

    HC      SYS_EVENT_CREATE, 3, 0
    mov     r12, rax
    NSP     SYS_NS_INSERT, s_ev, 8, r12
    EXPECT  0, 0x12
    NSP     SYS_NS_INSERT, s_ev, 8, r12
    EXPECT  -E_EXIST, 0x13
    HC      SYS_CLOSE, r12, 0
    EXPECT  0, 0x14

    NSP     SYS_NS_OPEN, s_ev, 8, ACCESS_SYNCHRONIZE
    test    rax, rax
    jg      1f
    mov     edi, 0x15
    jmp     ipc_exit
1:  mov     r13, rax
    HC      SYS_WAIT, r13, 0
    EXPECT  0, 0x16
    HC      SYS_EVENT_OP, r13, EVENT_SET
    EXPECT  -E_ACCES, 0x17
    NSP     SYS_NS_OPEN, s_ev, 8, 0x4
    EXPECT  -E_INVAL, 0x18

    NSP     SYS_NS_OPEN, s_nope, 5
    EXPECT  -E_NOENT, 0x19
    NSP     SYS_NS_OPEN, s_ghost, 17
    EXPECT  -E_NOENT, 0x1A
    NSP     SYS_NS_OPEN, s_evx, 11
    EXPECT  -E_NOTDIR, 0x1B
    NSP     SYS_NS_OPEN, s_test, 5
    test    rax, rax
    jg      2f
    mov     edi, 0x1C
    jmp     ipc_exit
2:  mov     rbx, rax
    NSP     SYS_NS_INSERT, s_link, 10, rbx
    EXPECT  -E_INVAL, 0x1D
    NSP     SYS_NS_QUERY, s_test, 5, 0, rbp
    EXPECT  0, 0x20
    cmp     byte ptr [rbp], 'e'
    je      3f
    mov     edi, 0x21
    jmp     ipc_exit
3:  EXPECT32 32, OBJ_EVENT, 0x22
    NSP     SYS_NS_QUERY, s_test, 5, 1, rbp
    EXPECT  -E_NOENT, 0x23
    NSP     SYS_NS_QUERY, s_ev, 8, 0, rbp
    EXPECT  -E_NOTDIR, 0x24

    NSP     SYS_NS_MKDIR, s_test, 0
    EXPECT  -E_INVAL, 0x25
    NSP     SYS_NS_MKDIR, s_rel, 4
    EXPECT  -E_INVAL, 0x26
    NSP     SYS_NS_MKDIR, s_dbl, 6
    EXPECT  -E_INVAL, 0x27
    NSP     SYS_NS_MKDIR, s_trail, 6
    EXPECT  -E_INVAL, 0x28
    NSP     SYS_NS_MKDIR, s_lower, 5
    EXPECT  0, 0x29
    NSP     SYS_NS_UNLINK, s_lower, 5
    EXPECT  0, 0x2A
    mov     rdi, USER_BASE + 0x50000
    mov     esi, 5
    SC      SYS_NS_MKDIR
    EXPECT  -E_FAULT, 0x2B

    SC      SYS_PORT_ALLOC
    mov     r14, rax
    HC      SYS_PORT_INSERT, r14, MSG_MAKE_SEND
    EXPECT  0, 0x30
    NSP     SYS_NS_PUBLISH, s_self, 14, r14
    EXPECT  0, 0x31
    NSP     SYS_NS_PUBLISH, s_self, 14, r14
    EXPECT  -E_EXIST, 0x32
    NSP     SYS_NS_LOOKUP_PORT, s_self, 14
    test    rax, rax
    jg      4f
    mov     edi, 0x33
    jmp     ipc_exit
4:  mov     r15, rax
    HDR     rbp,MSG_BITS(MSG_COPY_SEND,0),32,r15d,0,0x99,0
    mov     qword ptr [rbp + 24], 5
    mov     rdi, rbp
    xor     esi, esi
    SC      SYS_MSG_SEND
    EXPECT  0, 0x34
    RECV    r14d, 128, 256, 0
    EXPECT  32, 0x35
    EXPECT32 128+16, 0x99, 0x36
    NSP     SYS_NS_LOOKUP_PORT, s_ev, 8
    EXPECT  -E_INVAL, 0x37
    NSP     SYS_NS_LOOKUP_PORT, s_nope, 5
    EXPECT  -E_NOENT, 0x38
    HC      SYS_PORT_ALLOC, 0, 0
    mov     rbx, rax
    NSP     SYS_NS_PUBLISH, s_bare, 12, rbx
    EXPECT  -E_PERM, 0x39

    NSP     SYS_NS_UNLINK, s_test, 5
    EXPECT  -E_NOTEMPTY, 0x40
    NSP     SYS_NS_UNLINK, s_ev, 8
    EXPECT  0, 0x41
    HC      SYS_WAIT, r13, 0
    EXPECT  0, 0x42
    NSP     SYS_NS_OPEN, s_ev, 8
    EXPECT  -E_NOENT, 0x43
    NSP     SYS_NS_UNLINK, s_test, 5
    EXPECT  0, 0x44
    NSP     SYS_NS_UNLINK, s_test, 5
    EXPECT  -E_NOENT, 0x45
    NSP     SYS_NS_UNLINK, s_self, 14
    EXPECT  0, 0x46
    NSP     SYS_NS_UNLINK, s_root, 1
    EXPECT  -E_INVAL, 0x47
    xor     edi, edi
    jmp     ipc_exit

s_test:   .ascii "\\Test"
s_ev:     .ascii "\\Test\\ev"
s_evx:    .ascii "\\Test\\ev\\x"
s_nope:   .ascii "\\Nope"
s_ghost:  .ascii "\\Services\\ghost"
s_rel:    .ascii "Test"
s_dbl:    .ascii "\\\\Test"
s_trail:  .ascii "\\Test\\"
s_lower:  .ascii "\\test"
s_self:   .ascii "\\Services\\self"
s_bare:   .ascii "\\Services\\x"
s_root:   .ascii "\\"
s_link:   .ascii "\\Test\\link"

    .org UT_NS_SERVER
ut_ns_server:
    sub     rsp, 1024
    mov     rbp, rsp
    SC      SYS_PORT_ALLOC
    mov     r12, rax
    HC      SYS_PORT_INSERT, r12, MSG_MAKE_SEND
    EXPECT  0, 0x51
    NSP     SYS_NS_PUBLISH, e_echo, 14, r12
    EXPECT  0, 0x52
    RECV    r12d, 0, 512, 5000
    EXPECT  32, 0x53
    mov     r13d, dword ptr [rbp + 8]
    HDR     rbp+256,MSG_BITS(MSG_MOVE_SEND_ONCE,0),32,r13d,0,8,0
    mov     rax, [rbp + 24]
    inc     rax
    mov     [rbp + 256 + 24], rax
    lea     rdi, [rbp + 256]
    mov     rsi, -1
    SC      SYS_MSG_SEND
    EXPECT  0, 0x54
    NSP     SYS_NS_UNLINK, e_echo, 14
    EXPECT  0, 0x55
    xor     edi, edi
    jmp     ipc_exit
e_echo:   .ascii "\\Services\\echo"

    .org UT_NS_CLIENT
ut_ns_client:
    sub     rsp, 1024
    mov     rbp, rsp
    mov     r15d, 400
1:  NSP     SYS_NS_LOOKUP_PORT, c_echo, 14
    test    rax, rax
    jg      2f
    HC      SYS_SLEEP_MS, 5, 0
    dec     r15d
    jnz     1b
    mov     edi, 0x61
    jmp     ipc_exit
2:  mov     r12, rax
    SC      SYS_PORT_ALLOC
    mov     r13, rax
    HDR     rbp,MSG_BITS(MSG_COPY_SEND,MSG_MAKE_SEND_ONCE),32,r12d,r13d,7,0
    movabs  rax, 0x0102030405060708
    mov     [rbp + 24], rax
    mov     rdi, rbp
    mov     rsi, -1
    SC      SYS_MSG_SEND
    EXPECT  0, 0x62
    RECV    r13d, 256, 512, 5000
    EXPECT  32, 0x63
    EXPECT64 256+24, 0x0102030405060709, 0x64
    xor     edi, edi
    jmp     ipc_exit
c_echo:   .ascii "\\Services\\echo"

    .macro  SEC3 nr, h, off, which
    mov     rdi, \h
    lea     rsi, [rbp + \off]
    mov     rdx, \which
    SC      \nr
    .endm
    .macro  ZERO off, n
    lea     rdi, [rbp + \off]
    xor     eax, eax
    mov     ecx, \n
    rep     stosb
    .endm
    .macro  FAILIF_LE code
    test    rax, rax
    jg      9f
    mov     edi, \code
    jmp     ipc_exit
9:
    .endm

    .org UT_SEC_ALICE
ut_sec_alice:
    sub     rsp, 1024
    mov     rbp, rsp

    mov     rdi, rbp
    SC      SYS_TOKEN_INFO
    EXPECT  0, 0x10
    EXPECT32 0, 1000, 0x11
    EXPECT32 4, 0, 0x12
    EXPECT32 8, 1, 0x13
    EXPECT32 12, 2000, 0x14

    NSP     SYS_NS_MKDIR, sa_a, 2
    EXPECT  -E_ACCES, 0x15
    NSP     SYS_NS_MKDIR, sa_sec, 13
    EXPECT  0, 0x16

    HC      SYS_EVENT_CREATE, 3, 0
    FAILIF_LE 0x17
    mov     r12, rax
    SEC3    SYS_OBJ_GETSEC, r12, 256, 0
    EXPECT  0, 0x18
    EXPECT32 256+0, 1000, 0x19
    EXPECT32 256+4, 0, 0x1A
    EXPECT32 256+8, 0, 0x1B

    NSP     SYS_NS_INSERT, sa_ev, 16, r12
    EXPECT  0, 0x1C
    SEC3    SYS_OBJ_GETSEC, r12, 256, 0
    EXPECT  0, 0x1D
    EXPECT32 256+4, SD_DACL_PRESENT, 0x1E
    EXPECT32 256+8, 2, 0x1F
    EXPECT32 256+12+0, 0, 0x20
    EXPECT32 256+12+4, SID_EVERYONE, 0x21
    EXPECT32 256+24+4, 1000, 0x22

    HC      SYS_DUP, r12, ACCESS_SYNCHRONIZE
    FAILIF_LE 0x23
    mov     r13, rax
    SEC3    SYS_OBJ_GETSEC, r13, 256, 0
    EXPECT  -E_ACCES, 0x24
    ZERO    512, 160
    mov     dword ptr [rbp + 512 + 4], SD_DACL_PRESENT
    mov     dword ptr [rbp + 512 + 8], 1
    mov     dword ptr [rbp + 512 + 12 + 4], SID_EVERYONE
    mov     dword ptr [rbp + 512 + 12 + 8], ACCESS_SYNCHRONIZE
    SEC3    SYS_OBJ_SETSEC, r13, 512, SEC_DACL
    EXPECT  -E_ACCES, 0x25

    mov     byte ptr [rbp + 512 + 12], 7
    SEC3    SYS_OBJ_SETSEC, r12, 512, SEC_DACL
    EXPECT  -E_INVAL, 0x26
    mov     byte ptr [rbp + 512 + 12], 0
    mov     dword ptr [rbp + 512 + 12 + 8], 0x4
    SEC3    SYS_OBJ_SETSEC, r12, 512, SEC_DACL
    EXPECT  -E_INVAL, 0x27
    mov     dword ptr [rbp + 512 + 12 + 8], ACCESS_SYNCHRONIZE
    mov     dword ptr [rbp + 512 + 8], 13
    SEC3    SYS_OBJ_SETSEC, r12, 512, SEC_DACL
    EXPECT  -E_INVAL, 0x28
    mov     dword ptr [rbp + 512 + 8], 1
    SEC3    SYS_OBJ_SETSEC, r12, 512, 0
    EXPECT  -E_INVAL, 0x29
    mov     dword ptr [rbp + 512 + 0], 1001
    SEC3    SYS_OBJ_SETSEC, r12, 512, SEC_OWNER
    EXPECT  -E_PERM, 0x2A
    mov     dword ptr [rbp + 512 + 0], 2000
    SEC3    SYS_OBJ_SETSEC, r12, 512, SEC_OWNER
    EXPECT  0, 0x2B
    SEC3    SYS_OBJ_GETSEC, r12, 256, 0
    EXPECT  0, 0x2C
    EXPECT32 256+0, 2000, 0x2D
    mov     dword ptr [rbp + 512 + 0], 1000
    SEC3    SYS_OBJ_SETSEC, r12, 512, SEC_OWNER
    EXPECT  0, 0x2E
    mov     rdi, r12
    mov     rsi, USER_BASE + 0x50000
    mov     edx, SEC_DACL
    SC      SYS_OBJ_SETSEC
    EXPECT  -E_FAULT, 0x2F

    SEC3    SYS_OBJ_SETSEC, r12, 512, SEC_DACL
    EXPECT  0, 0x30
    SEC3    SYS_OBJ_GETSEC, r12, 256, 0
    EXPECT  0, 0x31
    EXPECT32 256+8, 1, 0x32
    EXPECT32 256+12+8, ACCESS_SYNCHRONIZE, 0x33

    NSP     SYS_NS_OPEN, sa_ev, 16, ACCESS_GENERIC_ALL
    EXPECT  -E_ACCES, 0x34
    NSP     SYS_NS_OPEN, sa_ev, 16, ACCESS_SYNCHRONIZE
    FAILIF_LE 0x35
    mov     r14, rax
    HC      SYS_WAIT, r14, 0
    EXPECT  0, 0x36
    NSP     SYS_NS_OPEN, sa_ev, 16, 0
    FAILIF_LE 0x37
    mov     r15, rax
    HC      SYS_EVENT_OP, r15, EVENT_SET
    EXPECT  -E_ACCES, 0x38
    SEC3    SYS_OBJ_GETSEC, r15, 256, 0
    EXPECT  0, 0x39
    SEC3    SYS_OBJ_SETSEC, r15, 512, SEC_OWNER
    EXPECT  -E_ACCES, 0x3A
    xor     edi, edi
    jmp     ipc_exit
sa_a:   .ascii "\\A"
sa_sec: .ascii "\\Services\\sec"
sa_ev:  .ascii "\\Services\\sec\\ev"

    .org UT_SEC_BOB
ut_sec_bob:
    sub     rsp, 1024
    mov     rbp, rsp

    mov     rdi, rbp
    SC      SYS_TOKEN_INFO
    EXPECT  0, 0x10
    EXPECT32 0, 1001, 0x11
    EXPECT32 8, 0, 0x12

    NSP     SYS_NS_OPEN, sb_ev, 16, ACCESS_SYNCHRONIZE
    FAILIF_LE 0x13
    mov     r12, rax
    HC      SYS_WAIT, r12, 0
    EXPECT  0, 0x14
    NSP     SYS_NS_OPEN, sb_ev, 16, EVENT_MODIFY_STATE
    EXPECT  -E_ACCES, 0x15
    NSP     SYS_NS_OPEN, sb_ev, 16, ACCESS_READ_CONTROL
    EXPECT  -E_ACCES, 0x16
    NSP     SYS_NS_OPEN, sb_ev, 16, 0
    FAILIF_LE 0x17
    mov     r13, rax
    SEC3    SYS_OBJ_GETSEC, r13, 256, 0
    EXPECT  -E_ACCES, 0x18
    HC      SYS_EVENT_OP, r13, EVENT_SET
    EXPECT  -E_ACCES, 0x19

    NSP     SYS_NS_UNLINK, sb_ev, 16
    EXPECT  -E_ACCES, 0x1A
    NSP     SYS_NS_MKDIR, sb_in, 17
    EXPECT  -E_ACCES, 0x1B
    NSP     SYS_NS_QUERY, sb_sec, 13, 0, rbp
    EXPECT  0, 0x1C
    cmp     byte ptr [rbp], 'e'
    je      1f
    mov     edi, 0x1D
    jmp     ipc_exit
1:
    NSP     SYS_NS_MKDIR, sb_mine, 16
    EXPECT  0, 0x1E
    NSP     SYS_NS_UNLINK, sb_mine, 16
    EXPECT  0, 0x1F
    xor     edi, edi
    jmp     ipc_exit
sb_sec:  .ascii "\\Services\\sec"
sb_ev:   .ascii "\\Services\\sec\\ev"
sb_in:   .ascii "\\Services\\sec\\bob"
sb_mine: .ascii "\\Services\\bobdir"

    .macro  OK code
    test    rax, rax
    jg      9f
    mov     edi, \code
    jmp     ipc_exit
9:
    .endm
    .macro  SEEK h, off, whence
    mov     rdi, \h
    mov     rsi, \off
    mov     rdx, \whence
    SC      SYS_FILE_SEEK
    .endm
    .macro  FIO nr, h, buf, len
    mov     rdi, \h
    lea     rsi, [rbp + \buf]
    mov     rdx, \len
    SC      \nr
    .endm

    .macro  FP nr, str, len, a=0, b=0
    lea     rdi, [rip + \str]
    mov     esi, offset \len
    mov     rdx, \a
    mov     r10, \b
    SC      \nr
    .endm

    .org UT_FS
ut_fs:
    sub     rsp, 1024
    mov     rbp, rsp
    test    rdi, rdi
    jz      fs_bob

    FP     SYS_FILE_OPEN, f_hello, f_hello_len, FILE_READ_DATA, FD_OPEN
    OK      0x10
    mov     r12, rax
    FIO     SYS_FILE_READ, r12, 0, 5
    EXPECT  5, 0x11
    cmp     dword ptr [rbp], 0x6C6C6568
    je      1f
    mov     edi, 0x12
    jmp     ipc_exit
1:  mov     rdi, r12
    lea     rsi, [rbp + 64]
    SC      SYS_FILE_STAT
    EXPECT  0, 0x13
    EXPECT64 64, 13, 0x14
    FIO     SYS_FILE_WRITE, r12, 0, 1
    EXPECT  -E_ACCES, 0x15
    SEEK    r12, 0, SEEK_END
    EXPECT  13, 0x16
    FIO     SYS_FILE_READ, r12, 0, 5
    EXPECT  0, 0x17

    FP     SYS_FILE_OPEN, f_new, f_new_len, FILE_READ_DATA | FILE_WRITE_DATA, FD_CREATE
    OK      0x18
    mov     r13, rax
    FP     SYS_FILE_OPEN, f_new, f_new_len, FILE_READ_DATA, FD_CREATE
    EXPECT  -E_EXIST, 0x19
    mov     dword ptr [rbp + 128], 0x64636261
    mov     word ptr [rbp + 132], 0x6665
    FIO     SYS_FILE_WRITE, r13, 128, 6
    EXPECT  6, 0x1A
    SEEK    r13, 0, SEEK_SET
    EXPECT  0, 0x1B
    FIO     SYS_FILE_READ, r13, 192, 6
    EXPECT  6, 0x1C
    cmp     dword ptr [rbp + 192], 0x64636261
    je      2f
    mov     edi, 0x1D
    jmp     ipc_exit
2:  HC      SYS_FILE_SETSIZE, r13, 3
    EXPECT  0, 0x1E
    mov     rdi, r13
    lea     rsi, [rbp + 64]
    SC      SYS_FILE_STAT
    EXPECT  0, 0x1F
    EXPECT64 64, 3, 0x1F

    FP     SYS_FS_MKDIR, f_dir, f_dir_len
    EXPECT  0, 0x20
    FP     SYS_FS_MKDIR, f_dir, f_dir_len
    EXPECT  -E_EXIST, 0x21
    lea     rdi, [rip + f_new]
    mov     esi, offset f_new_len
    lea     rdx, [rip + f_moved]
    mov     r10d, offset f_moved_len
    SC      SYS_FS_RENAME
    EXPECT  -E_BUSY, 0x22
    HC      SYS_CLOSE, r13, 0
    EXPECT  0, 0x22
    lea     rdi, [rip + f_new]
    mov     esi, offset f_new_len
    lea     rdx, [rip + f_moved]
    mov     r10d, offset f_moved_len
    SC      SYS_FS_RENAME
    EXPECT  0, 0x23
    FP     SYS_FS_STAT, f_new, f_new_len, rbp
    EXPECT  -E_NOENT, 0x24
    FP     SYS_FS_STAT, f_moved, f_moved_len, rbp
    EXPECT  0, 0x25
    EXPECT64 0, 3, 0x26

    FP     SYS_FILE_OPEN, f_root, f_root_len, FILE_READ_DATA, FD_OPEN | FO_DIRECTORY
    OK      0x27
    mov     r14, rax
    mov     rdi, r14
    xor     esi, esi
    lea     rdx, [rbp + 320]
    SC      SYS_FILE_READDIR
    EXPECT  0, 0x28
    mov     rdi, r14
    mov     esi, 100
    lea     rdx, [rbp + 320]
    SC      SYS_FILE_READDIR
    EXPECT  -E_NOENT, 0x29
    FIO     SYS_FILE_READ, r14, 0, 4
    EXPECT  -E_ISDIR, 0x2A

    FP     SYS_FILE_OPEN, f_nope, f_nope_len, FILE_READ_DATA, FD_OPEN
    EXPECT  -E_NOENT, 0x2B
    FP     SYS_FILE_OPEN, f_other, f_other_len, FILE_READ_DATA, FD_OPEN
    EXPECT  -E_NOENT, 0x2C
    FP     SYS_FILE_OPEN, f_hello, f_hello_len, 4, FD_OPEN
    EXPECT  -E_INVAL, 0x2D
    mov     rdi, r12
    mov     esi, 0x1000
    mov     edx, 4
    SC      SYS_FILE_READ
    EXPECT  -E_FAULT, 0x2E

    FP     SYS_FS_REMOVE, f_moved, f_moved_len
    EXPECT  0, 0x2F
    FP     SYS_FS_REMOVE, f_dir, f_dir_len
    EXPECT  0, 0x30
    FP     SYS_FS_REMOVE, f_dir, f_dir_len
    EXPECT  -E_NOENT, 0x31
    xor     edi, edi
    jmp     ipc_exit

fs_bob:
    FP     SYS_FILE_OPEN, f_hello, f_hello_len, FILE_READ_DATA, FD_OPEN
    OK      0x40
    mov     r12, rax
    FIO     SYS_FILE_READ, r12, 0, 5
    EXPECT  5, 0x41
    FP     SYS_FILE_OPEN, f_hello, f_hello_len, FILE_WRITE_DATA, FD_OPEN
    EXPECT  -E_ACCES, 0x42
    FP     SYS_FILE_OPEN, f_hello, f_hello_len, FILE_READ_DATA | FILE_WRITE_DATA, FD_OPEN
    EXPECT  -E_ACCES, 0x42
    FP     SYS_FS_MKDIR, f_dir, f_dir_len
    EXPECT  -E_ACCES, 0x43
    FP     SYS_FS_REMOVE, f_hello, f_hello_len
    EXPECT  -E_ACCES, 0x44
    FP     SYS_FILE_OPEN, f_new, f_new_len, FILE_READ_DATA, FD_CREATE
    EXPECT  -E_ACCES, 0x45
    FP     SYS_FS_STAT, f_hello, f_hello_len, rbp
    EXPECT  0, 0x46
    xor     edi, edi
    jmp     ipc_exit

f_hello: .ascii "\\Volumes\\t0\\hello.txt"
    .set    f_hello_len, . - f_hello
f_new:   .ascii "\\Volumes\\t0\\new.txt"
    .set    f_new_len, . - f_new
f_dir:   .ascii "\\Volumes\\t0\\dir"
    .set    f_dir_len, . - f_dir
f_moved: .ascii "\\Volumes\\t0\\dir\\moved.txt"
    .set    f_moved_len, . - f_moved
f_root:  .ascii "\\Volumes\\t0"
    .set    f_root_len, . - f_root
f_nope:  .ascii "\\Volumes\\t0\\nope.txt"
    .set    f_nope_len, . - f_nope
f_other: .ascii "\\Other\\x"
    .set    f_other_len, . - f_other

    .macro  DSEEK h, off, whence
    mov     rdi, \h
    mov     rsi, \off
    mov     rdx, \whence
    SC      SYS_DEV_SEEK
    .endm
    .macro  DIO h, code, inoff, inlen, outoff, outlen
    lea     rax, [rbp + \inoff]
    mov     [rbp + 512], rax
    mov     qword ptr [rbp + 520], \inlen
    lea     rax, [rbp + \outoff]
    mov     [rbp + 528], rax
    mov     qword ptr [rbp + 536], \outlen
    mov     rdi, \h
    mov     esi, \code
    lea     rdx, [rbp + 512]
    SC      SYS_DEV_IOCTL
    .endm
    .macro  DOPEN str, len, access
    lea     rdi, [rip + \str]
    mov     esi, offset \len
    mov     edx, \access
    SC      SYS_DEV_OPEN
    .endm
    .set    DRW, DEV_READ_DATA | DEV_WRITE_DATA | DEV_IOCTL

    .org UT_DEV
ut_dev:
    sub     rsp, 1024
    mov     rbp, rsp
    test    rdi, rdi
    jz      dev_bob

    mov     dword ptr [rbp], 0x6C6C6568
    mov     dword ptr [rbp + 4], 0x6564206F
    mov     dword ptr [rbp + 8], 0x65636976

    DOPEN   d_mem, d_mem_len, DRW
    OK      0x10
    mov     r12, rax
    FIO     SYS_DEV_WRITE, r12, 0, 12
    EXPECT  12, 0x11
    DSEEK   r12, 0, SEEK_SET
    EXPECT  0, 0x12
    FIO     SYS_DEV_READ, r12, 64, 12
    EXPECT  12, 0x13
    cmp     dword ptr [rbp + 64], 0x6C6C6568
    je      1f
    mov     edi, 0x14
    jmp     ipc_exit
1:  cmp     dword ptr [rbp + 72], 0x65636976
    je      2f
    mov     edi, 0x15
    jmp     ipc_exit
2:  DSEEK   r12, 0, SEEK_END
    EXPECT  4096, 0x16
    FIO     SYS_DEV_WRITE, r12, 0, 1
    EXPECT  -E_NOSPC, 0x17
    FIO     SYS_DEV_READ, r12, 64, 4
    EXPECT  0, 0x18
    DSEEK   r12, -1, SEEK_SET
    EXPECT  -E_INVAL, 0x19

    DIO     r12, DEVCTL_GETSIZE, 0, 0, 576, 8
    EXPECT  8, 0x1A
    EXPECT64 576, 4096, 0x1B
    DIO     r12, CNT_IOCTL_GET, 0, 0, 576, 24
    EXPECT  24, 0x1C
    EXPECT32 576, 1, 0x1D
    EXPECT32 584, 2, 0x1E
    EXPECT32 588, 2, 0x1F
    EXPECT32 592, 2, 0x20
    DIO     r12, 0x999, 0, 0, 0, 0
    EXPECT  -E_NOTTY, 0x21
    DIO     r12, DEVCTL_GETSIZE, 0, 0, 576, 5000
    EXPECT  -E_INVAL, 0x22
    mov     qword ptr [rbp + 520], 0
    mov     qword ptr [rbp + 528], 0x1000
    mov     qword ptr [rbp + 536], 8
    mov     rdi, r12
    mov     esi, DEVCTL_GETSIZE
    lea     rdx, [rbp + 512]
    SC      SYS_DEV_IOCTL
    EXPECT  -E_FAULT, 0x23

    mov     dword ptr [rbp + 96], 1
    DIO     r12, CNT_IOCTL_ROFS, 96, 4, 0, 0
    EXPECT  0, 0x24
    DSEEK   r12, 0, SEEK_SET
    FIO     SYS_DEV_WRITE, r12, 0, 12
    EXPECT  -E_ROFS, 0x25
    DSEEK   r12, 0, SEEK_SET
    FIO     SYS_DEV_READ, r12, 64, 12
    EXPECT  12, 0x26
    DOPEN   d_mem, d_mem_len, DRW
    EXPECT  -E_ROFS, 0x27
    mov     dword ptr [rbp + 96], 0
    DIO     r12, CNT_IOCTL_ROFS, 96, 4, 0, 0
    EXPECT  0, 0x28
    DIO     r12, DEVCTL_FLUSH, 0, 0, 0, 0
    EXPECT  -E_NOTSUP, 0x29

    DOPEN   d_mem, d_mem_len, DEV_READ_DATA
    OK      0x2A
    mov     r13, rax
    FIO     SYS_DEV_WRITE, r13, 0, 4
    EXPECT  -E_ACCES, 0x2B
    DIO     r13, DEVCTL_GETSIZE, 0, 0, 576, 8
    EXPECT  -E_ACCES, 0x2C
    HC      SYS_CLOSE, r13, 0
    EXPECT  0, 0x2D
    mov     rdi, r12
    mov     esi, 0x100000
    mov     edx, 4
    SC      SYS_DEV_READ
    EXPECT  -E_FAULT, 0x2E
    FIO     SYS_DEV_READ, r13, 64, 4
    EXPECT  -E_BADF, 0x2F

    DOPEN   d_nope, d_nope_len, DRW
    EXPECT  -E_NOENT, 0x30
    DOPEN   d_mem, d_mem_len, 0
    EXPECT  -E_INVAL, 0x31
    DOPEN   d_mem, d_mem_len, 0x100
    EXPECT  -E_INVAL, 0x32
    mov     edi, 0x100000
    mov     esi, 5
    mov     edx, 1
    SC      SYS_DEV_OPEN
    EXPECT  -E_FAULT, 0x33
    lea     rdi, [rip + d_mem]
    xor     esi, esi
    mov     edx, 1
    SC      SYS_DEV_OPEN
    EXPECT  -E_INVAL, 0x34

    DOPEN   d_null, d_null_len, DEV_READ_DATA | DEV_WRITE_DATA
    OK      0x35
    mov     r13, rax
    FIO     SYS_DEV_WRITE, r13, 0, 5
    EXPECT  5, 0x36
    FIO     SYS_DEV_READ, r13, 64, 5
    EXPECT  0, 0x37
    DSEEK   r13, 0, SEEK_SET
    EXPECT  -E_SPIPE, 0x38
    HC      SYS_CLOSE, r13, 0
    DOPEN   d_zero, d_zero_len, DEV_READ_DATA
    OK      0x39
    mov     r14, rax
    mov     qword ptr [rbp + 128], -1
    FIO     SYS_DEV_READ, r14, 128, 8
    EXPECT  8, 0x3A
    cmp     qword ptr [rbp + 128], 0
    je      3f
    mov     edi, 0x3B
    jmp     ipc_exit
3:  DOPEN   d_secret, d_secret_len, DRW
    OK      0x3C
    mov     r15, rax

    HC      SYS_CLOSE, r12, 0
    EXPECT  0, 0x3D
    FIO     SYS_DEV_READ, r12, 64, 4
    EXPECT  -E_BADF, 0x3E
    xor     edi, edi
    jmp     ipc_exit

dev_bob:
    DOPEN   d_mem, d_mem_len, DEV_READ_DATA
    OK      0x40
    mov     r12, rax
    FIO     SYS_DEV_READ, r12, 64, 12
    EXPECT  12, 0x41
    cmp     dword ptr [rbp + 64], 0x6C6C6568
    je      4f
    mov     edi, 0x42
    jmp     ipc_exit
4:  DOPEN   d_mem, d_mem_len, DEV_WRITE_DATA
    EXPECT  -E_ACCES, 0x43
    DOPEN   d_mem, d_mem_len, DRW
    EXPECT  -E_ACCES, 0x44
    DOPEN   d_mem, d_mem_len, DEV_IOCTL
    EXPECT  -E_ACCES, 0x45
    DOPEN   d_secret, d_secret_len, DEV_READ_DATA
    EXPECT  -E_ACCES, 0x46
    DOPEN   d_null, d_null_len, DRW
    OK      0x47
    FIO     SYS_DEV_WRITE, r12, 0, 4
    EXPECT  -E_ACCES, 0x48
    xor     edi, edi
    jmp     ipc_exit

d_mem:    .ascii "\\Device\\Mem0"
    .set    d_mem_len, . - d_mem
d_nope:   .ascii "\\Device\\Nope"
    .set    d_nope_len, . - d_nope
d_null:   .ascii "\\Device\\Null"
    .set    d_null_len, . - d_null
d_zero:   .ascii "\\Device\\Zero"
    .set    d_zero_len, . - d_zero
d_secret: .ascii "\\Device\\Secret"
    .set    d_secret_len, . - d_secret

    .section .note.GNU-stack,"",@progbits
