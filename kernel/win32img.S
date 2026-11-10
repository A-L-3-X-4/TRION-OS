    .section .rodata
    .balign 16
    .globl w32rt_start, w32rt_end, w32min_start, w32min_end, w32fix_start, w32fix_end
    .globl w32norel_start, w32norel_end, w32crt_start, w32crt_end
w32rt_start:
    .incbin "build/user/w32/rt.elf"
w32rt_end:
    .balign 16
w32min_start:
    .incbin "build/user/w32/w32min.exe"
w32min_end:
    .balign 16
w32fix_start:
    .incbin "build/user/w32/w32fix.exe"
w32fix_end:
    .balign 16
w32norel_start:
    .incbin "build/user/w32/w32norel.exe"
w32norel_end:
    .balign 16
w32crt_start:
    .incbin "build/user/w32/w32crt.exe"
w32crt_end:

    .section .note.GNU-stack,"",@progbits
