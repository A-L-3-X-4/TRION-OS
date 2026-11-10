    .section .rodata
    .balign 16
    .globl lxmin_start, lxmin_end, lxexec_start, lxexec_end, lxglibc_start, lxglibc_end
lxmin_start:
    .incbin "build/user/linux/lxmin.elf"
lxmin_end:
    .balign 16
lxexec_start:
    .incbin "build/user/linux/lxexec.elf"
lxexec_end:
    .balign 16
lxglibc_start:
    .incbin "build/user/linux/lxglibc.elf"
lxglibc_end:

    .section .note.GNU-stack,"",@progbits
