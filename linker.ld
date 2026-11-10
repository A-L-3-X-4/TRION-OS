OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

SECTIONS
{
    . = 1M;
    kernel_start = .;

    .multiboot : ALIGN(8) { KEEP(*(.multiboot)) }
    .text      : ALIGN(4096) { *(.text .text.*) }
    .rodata    : ALIGN(4096) { *(.rodata .rodata.*) }
    .data      : ALIGN(4096) { *(.data .data.*) }
    .bss       : ALIGN(4096) { *(COMMON) *(.bss .bss.*) }

    . = ALIGN(4096);
    kernel_end = .;

    /DISCARD/ : { *(.eh_frame) *(.comment) *(.note*) }
}
