MB2_MAGIC       equ 0xE85250D6
MB2_ARCH_I386   equ 0
MB2_BOOT_MAGIC  equ 0x36D76289
FB_WIDTH        equ 640
FB_HEIGHT       equ 480

section .multiboot progbits alloc noexec nowrite align=8
mb2_header:
    dd MB2_MAGIC
    dd MB2_ARCH_I386
    dd mb2_header_end - mb2_header
    dd 0x100000000 - (MB2_MAGIC + MB2_ARCH_I386 + (mb2_header_end - mb2_header))

    dw 5
    dw 1
    dd 20
    dd FB_WIDTH
    dd FB_HEIGHT
    dd 32
    align 8, db 0

    dw 0
    dw 0
    dd 8
mb2_header_end:

section .text
bits 32
global _start
extern kmain

_start:
    cli
    mov esp, stack_top
    ; Save GRUB's magic and info pointer into esi/edi before cpuid stomps all over eax and ebx
    mov esi, eax
    mov edi, ebx

    cmp esi, MB2_BOOT_MAGIC
    mov al, '1'
    jne boot_error

    pushfd
    pop eax
    mov ecx, eax
    ; Flip the ID bit in EFLAGS and if it sticks then cpuid exists, if it doesn't this is some ancient CPU and we're not doing this
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    cmp eax, ecx
    mov al, '2'
    je boot_error

    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    mov al, '3'
    jb boot_error
    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    mov al, '4'
    jz boot_error

    mov eax, pdpt
    or eax, 0x3
    mov [pml4], eax

    xor ecx, ecx
.pdpt_loop:
    mov eax, ecx
    shl eax, 12
    add eax, pd
    or eax, 0x3
    mov [pdpt + ecx * 8], eax
    inc ecx
    cmp ecx, 4
    jne .pdpt_loop

    xor ecx, ecx
; 2048 x 2 MiB pages identity mapped, so 4 GiB of "sure, why not", and it's the reason the framebuffer has to live below 4 GiB
.pd_loop:
    mov eax, ecx
    shl eax, 21
    or eax, 0x83
    mov [pd + ecx * 8], eax
    inc ecx
    cmp ecx, 2048
    jne .pd_loop

    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    mov eax, pml4
    mov cr3, eax

    mov ecx, 0xC0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    mov eax, cr0
    or eax, (1 << 31) | (1 << 16)
    mov cr0, eax

    lgdt [gdt64.ptr]
    ; A far jump because it's the only way to load a 64-bit code segment, the CPU insists
    jmp 0x08:long_mode_entry

boot_error:
    ; There's no printf in 32-bit land so poke "ERR:" and the code char into VGA text memory by hand like it's 1987
    mov dword [0xB8000], 0x4F524F45
    mov dword [0xB8004], 0x4F3A4F52
    mov byte  [0xB8008], al
    mov byte  [0xB8009], 0x4F
.hang:
    hlt
    jmp .hang

bits 64
long_mode_entry:
    mov ax, 0x10
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov edi, edi
    mov esi, esi
    mov rsp, stack_top
    xor ebp, ebp
    call kmain
.halt:
    cli
    hlt
    jmp .halt

section .rodata align=8
gdt64:
    dq 0
    dq 0x00209A0000000000
    dq 0x0000920000000000
.end:
.ptr:
    dw gdt64.end - gdt64 - 1
    dq gdt64

section .bss nobits align=4096
pml4:   resb 4096
pdpt:   resb 4096
pd:     resb 4096 * 4
align 16
stack_bottom:
    resb 64 * 1024
stack_top:
