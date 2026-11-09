# Reset stub for the install image (installer/SPEC.md, "Install image and trampoline").
#
# After a stock-firmware update, eboot copies the install image to flash 0x0 and
# boots the image header it finds at 0x1000. The install image places a mini app
# image there whose only segment is this stub, loaded to 0x40100000. The stub
# restarts the chip the way the SDK's system_restart_core() (Arduino's
# ESP.restart()) does, and the ROM then boots the RTOS bootloader at 0x0:
#
#   1. Cache_Read_Disable() (ROM 0x400047f0).
#   2. Clear DPORT 0x3ff00024 bits 3 and 4 (ICACHE_ENABLE_FIRST/SECOND_16K); only
#      a hardware reset clears them.
#   3. INTENABLE = 0, LITBASE = 0, rsync.
#   4. Jump to the ROM reset vector 0x40000080.
#
# The RTC software reset eboot's SWRST uses (0x60000700 bit 31) does not work
# here: on the bench the ROM printed "rst cause:3, boot mode:(3,7)" and stopped
# in ets_main.c, while this sequence went on into the RTOS bootloader and Havoc
# Radio. The sequence follows esp8266UartDownloadMode() in the Arduino core
# 3.0.2 (cores/esp8266/reboot_uart_dwnld.cpp), which mirrors system_restart_core().
#
# Literals come first because l32r only reaches backwards.

    .text
    .align  4
cache_read_disable:
    .word   0x400047f0
dport_icache:
    .word   0x3ff00024
icache_keep:
    .word   0xffffffe7
reset_vector:
    .word   0x40000080

    .global stub_entry
stub_entry:
    l32r    a2, cache_read_disable
    callx0  a2
    l32r    a2, dport_icache
    l32r    a4, icache_keep
    memw
    l32i    a3, a2, 0
    and     a3, a3, a4
    s32i    a3, a2, 0
    memw
    movi    a2, 0
    wsr.intenable a2
    wsr.litbase a2
    rsync
    l32r    a2, reset_vector
    jx      a2
