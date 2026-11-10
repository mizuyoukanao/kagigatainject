// Persistent settings, kept in the last flash sector and editable from the
// USB console. Read once at boot, before core1 starts.
#ifndef CONFIG_H
#define CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/regs/addressmap.h"
#include "pico.h"

// Flash layout, from the end of the 2 MiB: [log sectors ...][config].
#define FLASH_SECTOR 4096u
#define FLASH_CONFIG_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR)
#define FLASH_LOG_SECTORS 15u
#define FLASH_LOG_OFFSET (FLASH_CONFIG_OFFSET - FLASH_LOG_SECTORS * FLASH_SECTOR)

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  uint8_t pcie_ack;     // answer CMD8's PCIe availability bit
  uint8_t pcie_1v2;     // also claim PCIe 1.2 V support when asked
  uint8_t sd_mode;      // continue with SD identification after CMD8
  uint8_t check_crc;    // drop commands with a bad CRC7
  uint8_t dat3_pullup;  // the card's 50 k CD/DAT3 pull-up, for card detect
  uint8_t acmd41_busy;  // ACMD41 busy answers before ready
  uint8_t mid;          // CID manufacturer ID
  uint8_t cmd_phase;    // CMD launch phase, SD_PHASE_* (sd_responder.h)
  uint16_t rca;         // 0: derived from the chip ID
  uint8_t dat_phase;    // DAT0 launch phase, or SD_PHASE_SWEEP
  uint8_t reserved1;
  uint32_t c_size;      // CSD C_SIZE, capacity = (C_SIZE + 1) * 512 KiB
  uint16_t cmd6_fg[6];  // CMD6 support bits per function group 1..6
  uint8_t scr_4bit;     // SCR claims a 4-bit bus
  uint8_t scr_cmd;      // SCR CMD_SUPPORT: bit 0 CMD20, bit 1 CMD23
  // Pad drive of CMD / DAT0: 0 = 4 mA slow slew (reset default), 1 = 4 mA
  // fast, 2 = 8 mA fast, 3 = 12 mA fast. Zero in configs saved before.
  uint8_t cmd_drive;
  uint8_t dat_drive;
  uint32_t checksum;
} kg_config_t;

extern kg_config_t kg_config;

void config_load(void);
void config_defaults(kg_config_t *c);
void config_save(void);
void config_print(void);
// "name value" -> true if a setting was changed.
bool config_set(const char *name, const char *value);

// Erases and rewrites one sector (offset from the start of flash) with core0
// interrupts off. core1 keeps running: it never touches flash.
void flash_write_sector(uint32_t offset, const uint8_t data[FLASH_SECTOR]);
void flash_erase_sector(uint32_t offset);
static inline const void *flash_ptr(uint32_t offset) {
  return (const void *)(XIP_BASE + offset);
}

#endif  // CONFIG_H
