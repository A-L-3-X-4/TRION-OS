#include "abi.h"

    .intel_syntax noprefix
    .text

    .globl syscall_entry
syscall_entry:
    swapgs
    mov     gs:8, rsp
    mov     rsp, gs:0

    push    SEL_UDATA
    push    qword ptr gs:8
    push    r11
    push    SEL_UCODE
    push    rcx
    push    0
    push    0x80
    push    rax
    push    rcx
    push    rdx
    push    rbx
    push    rbp
    push    rsi
    push    rdi
    push    r8
    push    r9
    push    r10
    push    r11
    push    r12
    push    r13
    push    r14
    push    r15

    mov     rdi, rsp
    cld
    sti
    call    syscall_handler
    cli

    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     r11
    pop     r10
    pop     r9
    pop     r8
    pop     rdi
    pop     rsi
    pop     rbp
    pop     rbx
    pop     rdx
    pop     rcx
    pop     rax
    add     rsp, 16

    mov     rcx, [rsp]
    mov     r11, rcx
    shl     r11, 16
    sar     r11, 16
    cmp     r11, rcx
    jne     1f
    cmp     qword ptr [rsp + 8], SEL_UCODE
    jne     1f
    mov     r11, [rsp + 16]
    mov     rsp, [rsp + 24]
    swapgs
    sysretq
1:
    swapgs
    iretq

    .globl enter_user
enter_user:
    cli
    push    SEL_UDATA
    push    rsi
    push    0x202
    push    SEL_UCODE
    push    rdi
    mov     rdi, rdx
    xor     eax, eax
    xor     ebx, ebx
    xor     ecx, ecx
    xor     edx, edx
    xor     esi, esi
    xor     ebp, ebp
    xor     r8d, r8d
    xor     r9d, r9d
    xor     r10d, r10d
    xor     r11d, r11d
    xor     r12d, r12d
    xor     r13d, r13d
    xor     r14d, r14d
    xor     r15d, r15d
    swapgs
    iretq

    .section .note.GNU-stack,"",@progbits
