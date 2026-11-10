    .section .rodata
    .balign 16
    .globl user_tests_start
    .globl user_tests_end
user_tests_start:
    .incbin "build/user/tests.bin"
user_tests_end:

    .section .note.GNU-stack,"",@progbits
