/* HospitalOS x86-64 higher-half ELF linker script. */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(kmain)

/* Keep Limine requests, code, read-only data, and writable data separate. */
PHDRS
{
    limine_requests PT_LOAD FLAGS(0x06); /* Read/write */
    text            PT_LOAD FLAGS(0x05); /* Read/execute */
    rodata          PT_LOAD FLAGS(0x04); /* Read-only */
    data            PT_LOAD FLAGS(0x06); /* Read/write */
}

SECTIONS
{
    /* Limine requires x86-64 kernel program headers in the higher half. */
    . = 0xffffffff80000000;

    .limine_requests :
    {
        KEEP(*(.limine_requests_start))
        KEEP(*(.limine_requests))
        KEEP(*(.limine_requests_end))
    } :limine_requests

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .text :
    {
        KEEP(*(.text.entry))
        *(.text .text.*)
    } :text

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .rodata :
    {
        *(.rodata .rodata.*)
    } :rodata

    . = ALIGN(CONSTANT(MAXPAGESIZE));

    .data :
    {
        *(.data .data.*)
    } :data

    .bss :
    {
        *(.bss .bss.*)
        *(COMMON)
    } :data

    /DISCARD/ :
    {
        *(.eh_frame*)
        *(.note .note.*)
        *(.got .got.*)
    }
}
