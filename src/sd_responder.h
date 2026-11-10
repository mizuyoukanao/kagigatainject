// The card side of the SD bus: PIO for the CMD line, core1 for the protocol.
#ifndef SD_RESPONDER_H
#define SD_RESPONDER_H

#include "config.h"

// Launch phase of a transmitter (cmd_phase / dat_phase settings): bit 1 =
// launch from the CLK rising edge instead of the falling edge, bit 0 = CLK
// bypasses the PIO input synchroniser (~9 ns after the edge instead of ~22).
#define SD_PHASE_FAST 1u
#define SD_PHASE_RISING 2u
// dat_phase only: move to the next of the four phases at every CMD0.
#define SD_PHASE_SWEEP 4u

// core0, at boot, before the system clock is set up: configures the pins and
// the PIO from `cfg`, then starts core1. Runs on the boot ring oscillator, so
// it must not depend on clk_peri or on the timer being accurate.
void sd_responder_start(const kg_config_t *cfg);

// core0, console: DAT0 loopback self-test (see sd_responder.c). Refuses to
// run unless CLK is idle, i.e. the board is not in a powered slot.
void sd_responder_selftest(void);

#endif  // SD_RESPONDER_H
