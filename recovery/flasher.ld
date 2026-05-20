ENTRY(main)

SECTIONS {
    . = 0x00020000;
    .text : {
        *(.text*)
        *(.rodata*)
        *(.data*)
        . = ALIGN(4);
    }

    /DISCARD/ : {
        *(.dynamic)
        *(.got)
        *(.got.plt)
        *(.interp)
        *(.note*)
        *(.gnu*)
        *(.dynsym)
        *(.dynstr)
        *(.comment)
        *(.ARM.attributes)
        *(.bss*)
    }
}
