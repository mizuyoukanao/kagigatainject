// KagigataDisk pins as seen from the microSD side.
//
//   card pin | microSD (SD mode) | RP2354A
//   ---------+-------------------+--------
//   1        | DAT2              | NC
//   2        | CD/DAT3           | GPIO0
//   3        | CMD               | GPIO1
//   4        | VDD               | 3.3 V in
//   5        | CLK               | GPIO2
//   6        | VSS               | GND
//   7        | DAT0              | GPIO7
//   8        | DAT1              | NC
//
// DAT1/DAT2 are not connected, so only 1-bit SD mode would ever be possible.
// The handshake only needs CMD and CLK.
#ifndef BOARD_H
#define BOARD_H

#define PIN_SD_DAT3 0
#define PIN_SD_CMD 1
#define PIN_SD_CLK 2
#define PIN_SD_DAT0 7

// sd_cmd.pio reaches CLK as "pin 1", i.e. relative to CMD.
#if PIN_SD_CLK != PIN_SD_CMD + 1
#error "sd_cmd.pio expects CLK on the GPIO right after CMD"
#endif

#define PIN_LED1 19  // power / USB
#define PIN_LED2 14  // command activity
#define PIN_LED3 12  // PCIe acknowledged in CMD8

#endif  // BOARD_H
