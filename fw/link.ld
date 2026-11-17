SECTIONS
{
  . = 0x000B069C;   /* lueckenlos hinter dem Original-Image */
  .text : {
    /* 13.11.: Kopfblock (Sicherheitsnetz, guard.c) – alles, worauf das Original direkt zeigt, liegt vorn */
    KEEP(*(.head.hdr)) KEEP(*(.head.text)) KEEP(*(.head.ven)) KEEP(*(.head.data))
    __head_end = .;
    KEEP(*(.text.entry_usb)) KEEP(*(.text.entry_din)) KEEP(*(.text.entry_tick)) KEEP(*(.text.entry_loop))
    KEEP(*(.text.entry_audio)) KEEP(*(.text.entry_usbinit)) KEEP(*(.text.entry_ep)) KEEP(*(.text.entry_setup))
    KEEP(*(.text.entry_mttrk)) KEEP(*(.text.entry_mtfx)) KEEP(*(.text.entry_arpinit)) KEEP(*(.text.entry_chord))
    KEEP(*(.text.entry_menu))
    KEEP(*(.text.entry_nisname)) KEEP(*(.text.entry_nisgen))   /* Patch 018 (nis.c) */
    KEEP(*(.text.entry_uicb)) KEEP(*(.text.entry_uisend))       /* Patch 020 (usbin.c) */
    KEEP(*(.text.entry_slinit)) KEEP(*(.text.entry_slparam)) KEEP(*(.text.entry_grupd))   /* Patch 021 */
    KEEP(*(.text.entry_slload)) KEEP(*(.text.entry_slsave)) KEEP(*(.text.entry_slnew)) KEEP(*(.text.entry_slplay))   /* Patch 024 */
    KEEP(*(.text.entry_wvview)) KEEP(*(.text.entry_lrbtn)) KEEP(*(.text.entry_svkb)) KEEP(*(.text.entry_svchg)) KEEP(*(.text.entry_slnote))                                 /* Patch 023 */
    KEEP(*(.text.entry_ttdraw)) KEEP(*(.text.entry_hdr)) KEEP(*(.text.entry_cbpx)) KEEP(*(.text.entry_boot)) KEEP(*(.text.entry_ldcreate))                                 /* Patch 045 (tap.c) */
    *(.text*) *(.rodata*)
  }
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.ARM.attributes) *(.data*) *(.bss*) }
}
