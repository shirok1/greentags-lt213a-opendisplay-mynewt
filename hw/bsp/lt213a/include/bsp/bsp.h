#ifndef LT213A_BSP_H
#define LT213A_BSP_H
#include <stdint.h>
extern uint8_t _ram_start;
#define bssnz_t
#define sec_data_core __attribute__((section(".data.core")))
#define sec_bss_core __attribute__((section(".bss.core")))
#define RAM_SIZE 0x4000
#endif
