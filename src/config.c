#include "config.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"

#define CONFIG_MAGIC 0x4349474Bu  // "KGIC"
#define CONFIG_VERSION 4u

kg_config_t kg_config;

static uint8_t sector_buf[FLASH_SECTOR];

void flash_erase_sector(uint32_t offset) {
  uint32_t irq = save_and_disable_interrupts();
  flash_range_erase(offset, FLASH_SECTOR);
  restore_interrupts(irq);
}

void flash_write_sector(uint32_t offset, const uint8_t data[FLASH_SECTOR]) {
  uint32_t irq = save_and_disable_interrupts();
  flash_range_erase(offset, FLASH_SECTOR);
  // Header page last, so a sector cut short by power loss reads as invalid.
  flash_range_program(offset + FLASH_PAGE_SIZE, data + FLASH_PAGE_SIZE,
                      FLASH_SECTOR - FLASH_PAGE_SIZE);
  flash_range_program(offset, data, FLASH_PAGE_SIZE);
  restore_interrupts(irq);
}

static uint32_t checksum(const kg_config_t *c) {
  const uint8_t *p = (const uint8_t *)c;
  uint32_t h = 2166136261u;  // FNV-1a over everything before the checksum
  for (size_t i = 0; i < offsetof(kg_config_t, checksum); i++)
    h = (h ^ p[i]) * 16777619u;
  return h;
}

void config_defaults(kg_config_t *c) {
  memset(c, 0, sizeof *c);
  c->magic = CONFIG_MAGIC;
  c->version = CONFIG_VERSION;
  c->size = sizeof *c;
  c->pcie_ack = 1;
  c->pcie_1v2 = 0;  // microSD Express has no 1.2 V rail; VDD2 is 1.8 V
  c->sd_mode = 1;
  c->check_crc = 1;
  c->dat3_pullup = 1;
  c->acmd41_busy = 2;
  c->mid = 0x00;
  c->rca = 0;
  // The combination the Switch 2 recognised the SSD with. CMD launched from
  // the falling edge (the rising edge was rejected after CMD7). DAT0 driven
  // hard (12 mA, fast slew: with 4 mA slow, phase 2 lost the SCR) and moved
  // to the next phase on every attempt.
  c->cmd_phase = 0;
  c->dat_phase = 4;  // SD_PHASE_SWEEP
  c->cmd_drive = 0;
  c->dat_drive = 3;
  // A 512 GB SSD (IDEMA size, 1,000,215,216 sectors = 512,110,190,592 bytes,
  // e.g. WD PC SN530 512 GB), rounded down to whole 512 KiB:
  // (976771 + 1) * 512 KiB = 512,109,838,336 bytes, 344 KiB short.
  c->c_size = 976771;
  // Like a real card at 3.3 V: function 0 in every group, plus High Speed
  // (FG1 function 1). Bit 15 is set by real cards too. A host that switches
  // to High Speed then runs the bus at 50 MHz, which the PIO cannot follow
  // at 150 MHz; set cmd6_fg1 to 0x8001 to claim Default Speed only.
  for (int g = 0; g < 6; g++) c->cmd6_fg[g] = 0x8001;
  c->cmd6_fg[0] = 0x8003;
  // CMD20/CMD23 as every current SDXC card has. The SCR claims a 1-bit bus
  // only: claiming 4 bits (as real cards do) makes the Switch 2 spend its
  // last attempt on a 1-bit retry instead of asking for PCIe.
  c->scr_4bit = 0;
  c->scr_cmd = 0x3;
}

void config_load(void) {
  const kg_config_t *f = flash_ptr(FLASH_CONFIG_OFFSET);
  if (f->magic == CONFIG_MAGIC && f->version == CONFIG_VERSION &&
      f->size == sizeof *f && f->checksum == checksum(f)) {
    kg_config = *f;
  } else {
    config_defaults(&kg_config);
  }
}

void config_save(void) {
  kg_config.checksum = checksum(&kg_config);
  memset(sector_buf, 0xFF, sizeof sector_buf);
  memcpy(sector_buf, &kg_config, sizeof kg_config);
  flash_write_sector(FLASH_CONFIG_OFFSET, sector_buf);
}

typedef struct {
  const char *name;
  size_t offset;
  uint8_t width;  // bytes
  uint32_t max;
  const char *help;
} setting_t;

#define SETTING_AS(name, field, maxv, text) \
  {name, offsetof(kg_config_t, field), sizeof(((kg_config_t *)0)->field), maxv, text}
#define SETTING(field, maxv, text) SETTING_AS(#field, field, maxv, text)

static const setting_t settings[] = {
    SETTING(pcie_ack, 1, "set the PCIe availability bit in R7 (the handshake)"),
    SETTING(pcie_1v2, 1, "also claim PCIe 1.2 V support when the host asks"),
    SETTING(sd_mode, 1, "answer ACMD41/CMD2/CMD3/... after CMD8 (0: CMD0/CMD8 only)"),
    SETTING(check_crc, 1, "ignore commands with a bad CRC7, like a real card"),
    SETTING(dat3_pullup, 1, "pull CD/DAT3 up (card detect via DAT3)"),
    SETTING(acmd41_busy, 255, "ACMD41 answers reporting busy before ready"),
    SETTING(mid, 255, "CID manufacturer ID"),
    SETTING(cmd_phase, 3, "CMD launch: 0 fall, 1 fall+fast, 2 rise, 3 rise+fast"),
    SETTING(dat_phase, 4, "DAT0 launch: as cmd_phase, 4 = next phase at every CMD0"),
    SETTING(rca, 0xFFFF, "relative card address, 0 = derived from chip ID"),
    SETTING(c_size, 0x3FFFFF, "CSD C_SIZE; capacity = (C_SIZE+1) * 512 KiB"),
    SETTING(scr_4bit, 1, "SCR claims a 4-bit bus (ACMD6 to 4 bits accepted)"),
    SETTING(scr_cmd, 3, "SCR CMD_SUPPORT: bit 0 CMD20, bit 1 CMD23"),
    SETTING(cmd_drive, 3, "CMD pad: 0 4mA slow, 1 4mA fast, 2 8mA fast, 3 12mA fast"),
    SETTING(dat_drive, 3, "DAT0 pad: 0 4mA slow, 1 4mA fast, 2 8mA fast, 3 12mA fast"),
    SETTING_AS("cmd6_fg1", cmd6_fg[0], 0xFFFF, "CMD6 group 1 (access mode) support bits"),
    SETTING_AS("cmd6_fg2", cmd6_fg[1], 0xFFFF, "CMD6 group 2 (command system) support bits"),
    SETTING_AS("cmd6_fg3", cmd6_fg[2], 0xFFFF, "CMD6 group 3 (driver strength) support bits"),
    SETTING_AS("cmd6_fg4", cmd6_fg[3], 0xFFFF, "CMD6 group 4 (power limit) support bits"),
    SETTING_AS("cmd6_fg5", cmd6_fg[4], 0xFFFF, "CMD6 group 5 support bits"),
    SETTING_AS("cmd6_fg6", cmd6_fg[5], 0xFFFF, "CMD6 group 6 support bits"),
};

static uint32_t get_field(const setting_t *s) {
  const uint8_t *p = (const uint8_t *)&kg_config + s->offset;
  uint32_t v = 0;
  memcpy(&v, p, s->width);  // little-endian
  return v;
}

void config_print(void) {
  for (size_t i = 0; i < count_of(settings); i++) {
    const setting_t *s = &settings[i];
    printf("  %-12s = 0x%-6lx (%lu)  %s\n", s->name,
           (unsigned long)get_field(s), (unsigned long)get_field(s), s->help);
  }
}

bool config_set(const char *name, const char *value) {
  for (size_t i = 0; i < count_of(settings); i++) {
    const setting_t *s = &settings[i];
    if (strcmp(s->name, name) != 0) continue;
    char *end;
    unsigned long v = strtoul(value, &end, 0);
    if (end == value || *end || v > s->max) {
      printf("bad value for %s (max %lu)\n", name, (unsigned long)s->max);
      return false;
    }
    uint32_t v32 = (uint32_t)v;
    memcpy((uint8_t *)&kg_config + s->offset, &v32, s->width);
    return true;
  }
  printf("unknown setting '%s'\n", name);
  return false;
}
