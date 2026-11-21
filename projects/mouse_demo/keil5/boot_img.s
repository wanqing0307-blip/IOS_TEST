@ Bootloader image for the SE73_BOOT target.
@ The raw vendor bin is placed at the start of the image, so downloading
@ this target programs the bootloader at flash address 0x00000000.
    .section RESET, "a"
    .incbin "../../../SE73_BootLoader_DLS853_20260330.bin"
