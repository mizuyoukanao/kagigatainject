#include "sdlog.h"

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "hardware/clocks.h"
#include "pico/stdio_usb.h"
#include "pico/time.h"
#include "sd_proto.h"

// Not zeroed at boot (only entries below sdlog_count are ever read), which
// keeps crt0 short: the host may start talking within milliseconds.
sdlog_entry_t __uninitialized_ram(sdlog_buf)[SDLOG_RAM_ENTRIES];
volatile uint32_t sdlog_count;
volatile bool sdlog_sysclk_ready;

// ---------------------------------------------------------------------------
// Flash sessions: one sector per boot that saw any command, used as a ring.

#define SESSION_MAGIC 0x314C474Bu  // "KGL1"

typedef struct {
  uint32_t magic;
  uint32_t seq;           // increases by one per saved boot
  uint32_t total;         // commands seen in that boot
  uint16_t nsaved;        // entries stored below
  uint16_t entry_size;
  uint32_t flags;         // SESSION_F_*
  uint32_t config_word;   // pcie_ack | pcie_1v2 << 1 | sd_mode << 2 | ...
  uint32_t reserved[2];
} session_hdr_t;

#define SESSION_F_PCIE_ACK 1u
#define SESSION_ENTRIES \
  ((FLASH_SECTOR - sizeof(session_hdr_t)) / sizeof(sdlog_entry_t))

_Static_assert(sizeof(session_hdr_t) == 32, "session header layout");
_Static_assert(sizeof(sdlog_entry_t) == 20, "log entry layout");

static uint32_t session_sector;  // index into the ring for this boot
static uint32_t session_seq;
static uint32_t saved_entries, saved_total;
static bool saved_once;
static uint32_t seen_count;
static absolute_time_t last_change, last_save;
static uint8_t sector_buf[FLASH_SECTOR];
static uint32_t printed;
static bool pcie_acked;

static const session_hdr_t *session_at(uint32_t sector) {
  return flash_ptr(FLASH_LOG_OFFSET + sector * FLASH_SECTOR);
}

static bool session_valid(const session_hdr_t *h) {
  return h->magic == SESSION_MAGIC && h->entry_size == sizeof(sdlog_entry_t) &&
         h->nsaved <= SESSION_ENTRIES;
}

void sdlog_flash_init(void) {
  uint32_t best_seq = 0, best = FLASH_LOG_SECTORS - 1;
  for (uint32_t i = 0; i < FLASH_LOG_SECTORS; i++) {
    const session_hdr_t *h = session_at(i);
    if (session_valid(h) && h->seq >= best_seq) {
      best_seq = h->seq;
      best = i;
    }
  }
  session_sector = (best + 1) % FLASH_LOG_SECTORS;
  session_seq = best_seq + 1;
  last_change = last_save = get_absolute_time();
}

static void save_session(uint32_t total) {
  uint32_t n = total < SESSION_ENTRIES ? total : SESSION_ENTRIES;
  session_hdr_t h = {
      .magic = SESSION_MAGIC,
      .seq = session_seq,
      .total = total,
      .nsaved = (uint16_t)n,
      .entry_size = sizeof(sdlog_entry_t),
      .flags = pcie_acked ? SESSION_F_PCIE_ACK : 0,
      .config_word = kg_config.pcie_ack | kg_config.pcie_1v2 << 1 |
                     kg_config.sd_mode << 2 | kg_config.check_crc << 3 |
                     kg_config.dat3_pullup << 4,
  };
  memset(sector_buf, 0xFF, sizeof sector_buf);
  memcpy(sector_buf, &h, sizeof h);
  memcpy(sector_buf + sizeof h, sdlog_buf, n * sizeof(sdlog_entry_t));
  flash_write_sector(FLASH_LOG_OFFSET + session_sector * FLASH_SECTOR,
                     sector_buf);
  saved_entries = n;
  saved_total = total;
  saved_once = true;
  last_save = get_absolute_time();
}

// The slot may lose power at any moment, so save soon after the traffic
// stops (typically right after CMD8, when the host switches to PCIe), and
// at most once a second while it keeps going.
#define SAVE_QUIET_MS 30
#define SAVE_MIN_INTERVAL_MS 1000

void sdlog_service(void) {
  uint32_t n = sdlog_count;
  absolute_time_t now = get_absolute_time();
  if (n != seen_count) {
    for (uint32_t i = seen_count; i < n && i < SDLOG_RAM_ENTRIES; i++)
      if (sdlog_buf[i].flags & SD_LOGF_PCIE_ACK) pcie_acked = true;
    seen_count = n;
    last_change = now;
  }
  if (n == 0 || (saved_once && n == saved_total)) return;

  bool quiet = absolute_time_diff_us(last_change, now) >= SAVE_QUIET_MS * 1000;
  bool due = absolute_time_diff_us(last_save, now) >= SAVE_MIN_INTERVAL_MS * 1000;
  uint32_t storable = n < SESSION_ENTRIES ? n : SESSION_ENTRIES;
  // Once the sector is full only the total changes; refresh that on quiet.
  if ((quiet && (!saved_once || due || storable != saved_entries)) ||
      (due && storable != saved_entries))
    save_session(n);
}

bool sdlog_pcie_acked(void) { return pcie_acked; }

// ---------------------------------------------------------------------------
// Printing

static const char *state_name(uint8_t s) {
  static const char *names[] = {"idle", "ready", "ident", "stby", "tran",
                                "data", "rcv",   "prg",   "dis"};
  if (s < count_of(names)) return names[s];
  return s == SD_STATE_INA ? "ina" : "?";
}

static const char *resp_name(uint8_t k) {
  static const char *names[] = {"--", "R1", "R1b", "R2", "R3", "R6", "R7"};
  return k < count_of(names) ? names[k] : "?";
}

static const char *cmd_name(uint8_t index, bool app) {
  if (app) {
    switch (index) {
      case 6: return "SET_BUS_WIDTH";
      case 13: return "SD_STATUS";
      case 22: return "SEND_NUM_WR_BLOCKS";
      case 23: return "SET_WR_BLK_ERASE_COUNT";
      case 41: return "SD_SEND_OP_COND";
      case 42: return "SET_CLR_CARD_DETECT";
      case 51: return "SEND_SCR";
    }
  }
  switch (index) {
    case 0: return "GO_IDLE_STATE";
    case 1: return "SEND_OP_COND(MMC)";
    case 2: return "ALL_SEND_CID";
    case 3: return "SEND_RELATIVE_ADDR";
    case 4: return "SET_DSR";
    case 5: return "IO_SEND_OP_COND(SDIO)";
    case 6: return "SWITCH_FUNC";
    case 7: return "SELECT_CARD";
    case 8: return "SEND_IF_COND";
    case 9: return "SEND_CSD";
    case 10: return "SEND_CID";
    case 11: return "VOLTAGE_SWITCH";
    case 12: return "STOP_TRANSMISSION";
    case 13: return "SEND_STATUS";
    case 15: return "GO_INACTIVE_STATE";
    case 16: return "SET_BLOCKLEN";
    case 17: return "READ_SINGLE_BLOCK";
    case 18: return "READ_MULTIPLE_BLOCK";
    case 19: return "SEND_TUNING_BLOCK";
    case 20: return "SPEED_CLASS_CONTROL";
    case 23: return "SET_BLOCK_COUNT";
    case 24: return "WRITE_BLOCK";
    case 25: return "WRITE_MULTIPLE_BLOCK";
    case 32: return "ERASE_WR_BLK_START";
    case 33: return "ERASE_WR_BLK_END";
    case 38: return "ERASE";
    case 48: return "READ_EXTR_SINGLE";
    case 49: return "WRITE_EXTR_SINGLE";
    case 55: return "APP_CMD";
    case 56: return "GEN_CMD";
    case 58: return "READ_EXTR_MULTI";
    case 59: return "WRITE_EXTR_MULTI";
  }
  return "";
}

// The SD clock while the command came in, from 16 bits' worth of clk_sys
// cycles (+-3 cycles of polling jitter: about +-3 % at 25 MHz).
static void print_clock(const sdlog_entry_t *e) {
  if (!(e->flags & SDLOG_F_SYSCLK)) {
    printf("  clk=? (boot osc)");
    return;
  }
  if (e->clk16_cycles < 16) {  // both words were already queued
    printf("  clk=?");
    return;
  }
  uint32_t khz = (uint32_t)((uint64_t)clock_get_hz(clk_sys) * 16u /
                            e->clk16_cycles / 1000u);
  if (khz >= 1000)
    printf("  clk=%lu.%luMHz", (unsigned long)(khz / 1000),
           (unsigned long)(khz % 1000 / 100));
  else
    printf("  clk=%lukHz", (unsigned long)khz);
}

void sdlog_print_entry(uint32_t i, const sdlog_entry_t *e) {
  bool app = e->flags & SD_LOGF_APP;
  printf("%5lu %7lu.%03lums %s%-2u %-22s arg=%08lx", (unsigned long)i,
         (unsigned long)(e->t_us / 1000u), (unsigned long)(e->t_us % 1000u),
         app ? "ACMD" : " CMD", e->index, cmd_name(e->index, app),
         (unsigned long)e->arg);
  if (e->flags & SD_LOGF_BAD_FRAME) {
    printf("  [bad frame]\n");
    return;
  }
  if (!(e->flags & SD_LOGF_CRC_OK)) printf(" crc=BAD");
  if (e->resp_kind == SD_RESP_NONE)
    printf("  -> (none)");
  else if (e->resp_kind == SD_RESP_R2)
    printf("  -> R2");
  else
    printf("  -> %-3s %08lx", resp_name(e->resp_kind), (unsigned long)e->resp);
  if (e->flags & SD_LOGF_PCIE_ACK) printf(" [PCIe ACK]");
  if (e->flags & SD_LOGF_DATA) printf(" +DAT0 block");
  if (e->extra & SDLOG_X_DAT_SKIPPED) printf(" [4-bit: no block]");
  if (e->flags & SD_LOGF_ILLEGAL) printf(" [illegal]");
  printf("  st=%s%s", state_name(e->state),
         (e->extra & SDLOG_X_4BIT) ? " 4bit" : "");
  print_clock(e);
  // DAT0 phase: at each attempt's start and wherever a block went out.
  if (e->index == 0 || (e->flags & SD_LOGF_DATA))
    printf("  dph=%u", e->dat_phase);
  printf("\n");
}

void sdlog_print_new(void) {
  uint32_t n = sdlog_count;
  if (!stdio_usb_connected()) {
    printed = n;  // only show what arrives while someone is watching
    return;
  }
  for (; printed < n; printed++) {
    if (printed >= SDLOG_RAM_ENTRIES) {
      printf("(RAM log full, %lu more commands not stored)\n",
             (unsigned long)(n - printed));
      printed = n;
      break;
    }
    sdlog_print_entry(printed, &sdlog_buf[printed]);
  }
}

void sdlog_print_ram(void) {
  uint32_t n = sdlog_count;
  printf("this boot: %lu commands%s\n", (unsigned long)n,
         pcie_acked ? ", PCIe acknowledged" : "");
  for (uint32_t i = 0; i < n && i < SDLOG_RAM_ENTRIES; i++)
    sdlog_print_entry(i, &sdlog_buf[i]);
}

// Sessions sorted newest first: which = 0 is the most recent saved boot.
static int sessions_by_age(uint32_t order[FLASH_LOG_SECTORS]) {
  int n = 0;
  for (uint32_t i = 0; i < FLASH_LOG_SECTORS; i++)
    if (session_valid(session_at(i))) order[n++] = i;
  for (int a = 1; a < n; a++)
    for (int b = a; b > 0 && session_at(order[b])->seq >
                                 session_at(order[b - 1])->seq; b--) {
      uint32_t t = order[b];
      order[b] = order[b - 1];
      order[b - 1] = t;
    }
  return n;
}

void sdlog_print_history(int which) {
  uint32_t order[FLASH_LOG_SECTORS];
  int n = sessions_by_age(order);
  if (n == 0) {
    printf("no saved sessions\n");
    return;
  }
  if (which < 0) {
    printf("saved sessions (newest first):\n");
    for (int i = 0; i < n; i++) {
      const session_hdr_t *h = session_at(order[i]);
      const sdlog_entry_t *e = (const sdlog_entry_t *)(h + 1);
      printf("  [%d] boot #%lu: %lu commands (%u stored)%s%s, first at %lu us\n",
             i, (unsigned long)h->seq, (unsigned long)h->total, h->nsaved,
             (h->flags & SESSION_F_PCIE_ACK) ? ", PCIe ACK" : "",
             h->seq == session_seq ? " (this boot)" : "",
             h->nsaved ? (unsigned long)e[0].t_us : 0ul);
    }
    printf("'hist N' prints one\n");
    return;
  }
  if (which >= n) {
    printf("no session [%d]\n", which);
    return;
  }
  const session_hdr_t *h = session_at(order[which]);
  const sdlog_entry_t *e = (const sdlog_entry_t *)(h + 1);
  printf("boot #%lu: %lu commands, %u stored, config pcie_ack=%lu pcie_1v2=%lu "
         "sd_mode=%lu check_crc=%lu dat3_pullup=%lu\n",
         (unsigned long)h->seq, (unsigned long)h->total, h->nsaved,
         (unsigned long)(h->config_word & 1), (unsigned long)(h->config_word >> 1 & 1),
         (unsigned long)(h->config_word >> 2 & 1), (unsigned long)(h->config_word >> 3 & 1),
         (unsigned long)(h->config_word >> 4 & 1));
  for (uint32_t i = 0; i < h->nsaved; i++) sdlog_print_entry(i, &e[i]);
}

void sdlog_erase_history(void) {
  for (uint32_t i = 0; i < FLASH_LOG_SECTORS; i++)
    if (session_at(i)->magic != 0xFFFFFFFFu)
      flash_erase_sector(FLASH_LOG_OFFSET + i * FLASH_SECTOR);
  saved_once = false;  // this boot's log goes back into sector 0
  saved_total = saved_entries = 0;
  session_sector = 0;
  session_seq = 1;
}
