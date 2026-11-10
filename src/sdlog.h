// Command log: core1 appends every frame it handles to a RAM buffer, core0
// prints it over USB and saves each boot's log to a flash sector, since the
// board is usually powered by the console slot and loses RAM on removal.
#ifndef SDLOG_H
#define SDLOG_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/sync.h"
#include "hardware/timer.h"
#include "pico.h"

typedef struct {
  uint32_t t_us;      // time since boot (approximate before clocks_init)
  uint32_t arg;       // command argument
  uint32_t resp;      // 32-bit response payload (48-bit responses only)
  uint8_t index;      // command index
  uint8_t flags;      // SD_LOGF_*
  uint8_t resp_kind;  // sd_resp_kind_t
  uint8_t state;      // card state after the command
  // clk_sys cycles between the receiver's two words, i.e. for the last 16
  // bits of the command: 16 SD clocks. Saturates at 0xFFFF.
  uint16_t clk16_cycles;
  uint8_t dat_phase;  // DAT0 launch phase in use (SD_PHASE_* bits)
  uint8_t extra;      // SDLOG_X_*
} sdlog_entry_t;

#define SDLOG_X_4BIT 0x01u         // bus was 4 bits wide after the command
#define SDLOG_X_DAT_SKIPPED 0x02u  // a block was due but could not be sent

// Set in sdlog_entry_t.flags (above the SD_LOGF_* bits) when clk_sys was
// already the PLL, so clk16_cycles converts to an SD clock frequency.
#define SDLOG_F_SYSCLK 0x80u

#define SDLOG_RAM_ENTRIES 2048u

extern sdlog_entry_t sdlog_buf[SDLOG_RAM_ENTRIES];
// Commands seen this boot; entries past SDLOG_RAM_ENTRIES are counted only.
extern volatile uint32_t sdlog_count;
// core0 sets this once clk_sys runs from the PLL.
extern volatile bool sdlog_sysclk_ready;

// core1. Inline so it stays in RAM with the caller.
static __force_inline void sdlog_add(uint32_t arg, uint32_t resp,
                                     uint8_t index, uint8_t flags,
                                     uint8_t resp_kind, uint8_t state,
                                     uint32_t clk16_cycles,
                                     uint8_t dat_phase, uint8_t extra) {
  uint32_t n = sdlog_count;
  if (n < SDLOG_RAM_ENTRIES) {
    sdlog_entry_t *e = &sdlog_buf[n];
    e->t_us = timer_hw->timerawl;
    e->arg = arg;
    e->resp = resp;
    e->index = index;
    e->flags = flags | (sdlog_sysclk_ready ? SDLOG_F_SYSCLK : 0u);
    e->clk16_cycles = clk16_cycles > 0xFFFFu ? 0xFFFFu : (uint16_t)clk16_cycles;
    e->dat_phase = dat_phase;
    e->extra = extra;
    e->resp_kind = resp_kind;
    e->state = state;
    __dmb();  // entry before count, for core0
  }
  sdlog_count = n + 1;
}

// core0 only below.

// Finds the flash sector this boot will save into.
void sdlog_flash_init(void);
// Saves the log to flash when it has changed and the bus has gone quiet.
void sdlog_service(void);

void sdlog_print_entry(uint32_t i, const sdlog_entry_t *e);
// Prints new entries live; call from the main loop.
void sdlog_print_new(void);
void sdlog_print_ram(void);
void sdlog_print_history(int which);  // -1: list sessions
void sdlog_erase_history(void);
bool sdlog_pcie_acked(void);

#endif  // SDLOG_H
