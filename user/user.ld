OUTPUT_FORMAT(binary)
ENTRY(ut_cpl)

SECTIONS
{
    . = 0x8000001000;
    .text : { *(.text .text.*) }
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    /DISCARD/ : { *(.note*) *(.comment) *(.eh_frame) }
}
