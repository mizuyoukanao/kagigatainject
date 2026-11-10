// Board header for the KagigataDisk (bare RP2354A). pico-sdk has no board
// file for it; it is an RP2350A (QFN60) with 2 MiB of flash on-die, reached
// through the same QMI/XIP interface as the external chip on a Pico 2, so
// pico2.h applies apart from the flash size.
//
// -----------------------------------------------------
// NOTE: THIS HEADER IS ALSO INCLUDED BY ASSEMBLER SO
//       SHOULD ONLY CONSIST OF PREPROCESSOR DIRECTIVES
// -----------------------------------------------------
#ifndef _BOARDS_KAGIGATADISK_H
#define _BOARDS_KAGIGATADISK_H

pico_board_cmake_set(PICO_PLATFORM, rp2350)

// Before pico2.h, whose own #ifndef default (4 MiB) then leaves it alone.
pico_board_cmake_set_default(PICO_FLASH_SIZE_BYTES, (2 * 1024 * 1024))
#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (2 * 1024 * 1024)
#endif

#include "boards/pico2.h"

#endif
