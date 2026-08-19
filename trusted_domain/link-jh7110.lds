OUTPUT_ARCH("riscv")
ENTRY(_start)

/* 6.6 AMP SDK RT-Thread code region: 0x6e800000-0x6f000000. */
__stack_size = 0x4000;

MEMORY
{
    rom (rxa) : ORIGIN = 0x6e800000, LENGTH = 8M
    ram (wxa) : ORIGIN = 0x6e800000, LENGTH = 8M
}

SECTIONS
{
    .init : { KEEP (*(SORT_NONE(.init))) } >rom

    .text :
    {
        *(.text.startup .text.startup.*)
        *(.text .text.*)
        *(.rodata .rodata.*)
        . = ALIGN(8);
        _shell_command_start = .;
        KEEP (*(shellCommand))
        _shell_command_end = .;
    } >rom

    . = ALIGN(8);
    PROVIDE(_data_lma = .);

    .data :
    {
        PROVIDE(_data = .);
        *(.data .data.*)
        . = ALIGN(8);
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*)
    } >rom

    . = ALIGN(8);
    PROVIDE(_edata = .);

    .bss (NOLOAD) :
    {
        PROVIDE(__bss_start = .);
        *(.sbss .sbss.* .bss .bss.* COMMON)
        . = ALIGN(8);
        PROVIDE(__bss_end = .);
    } >rom

    .stack (NOLOAD) :
    {
        . = ORIGIN(rom) + LENGTH(rom) - __stack_size;
        PROVIDE(heap_end = .);
        . += __stack_size;
        PROVIDE(_sp = .);
    } >rom

    PROVIDE(_end = .);
    PROVIDE(end = .);
    PROVIDE(heap_start = .);
}
