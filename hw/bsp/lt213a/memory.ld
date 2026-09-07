/* Standalone image: no SoftDevice, MCUboot, or image header. */
MEMORY {
    FLASH (rx) : ORIGIN = 0x00000000, LENGTH = 118K
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 16K
}
_imghdr_size = 0;
/* NimBLE allocates its GATT tables from the heap during sysinit/startup. */
ASSERT(__HeapLimit - __HeapBase >= 2048, "less than 2 KiB runtime heap reserve")
