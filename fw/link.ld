SECTIONS
{
  . = 0x000B069C;   /* lueckenlos hinter dem Original-Image */
  .text : {
    KEEP(*(.text.entry_usb)) KEEP(*(.text.entry_din)) KEEP(*(.text.entry_tick)) KEEP(*(.text.entry_loop))
    KEEP(*(.text.entry_audio)) KEEP(*(.text.entry_usbinit)) KEEP(*(.text.entry_ep)) KEEP(*(.text.entry_setup))
    KEEP(*(.text.entry_mttrk)) KEEP(*(.text.entry_mtfx)) KEEP(*(.text.entry_arpinit)) KEEP(*(.text.entry_chord))
    KEEP(*(.text.entry_menu))
    KEEP(*(.text.entry_nisname)) KEEP(*(.text.entry_nisgen))   /* Patch 018 (nis.c) */
    KEEP(*(.text.entry_uicb)) KEEP(*(.text.entry_uisend))       /* Patch 020 (usbin.c) */
    KEEP(*(.text.entry_lrbtn))                                 /* Patch 023 */
    *(.text*) *(.rodata*)
  }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.ARM.attributes) *(.data*) *(.bss*) }
}
