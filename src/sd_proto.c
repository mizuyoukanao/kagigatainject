#include "sd_proto.h"

// CRC7 (x^7 + x^3 + 1), kept left-aligned in a byte so one lookup per input
// byte suffices. In RAM: core1 reads it while core0 may be writing flash.
static const uint8_t SD_RAMDATA crc7_table[256] = {
    0x00, 0x12, 0x24, 0x36, 0x48, 0x5a, 0x6c, 0x7e, 0x90, 0x82, 0xb4, 0xa6,
    0xd8, 0xca, 0xfc, 0xee, 0x32, 0x20, 0x16, 0x04, 0x7a, 0x68, 0x5e, 0x4c,
    0xa2, 0xb0, 0x86, 0x94, 0xea, 0xf8, 0xce, 0xdc, 0x64, 0x76, 0x40, 0x52,
    0x2c, 0x3e, 0x08, 0x1a, 0xf4, 0xe6, 0xd0, 0xc2, 0xbc, 0xae, 0x98, 0x8a,
    0x56, 0x44, 0x72, 0x60, 0x1e, 0x0c, 0x3a, 0x28, 0xc6, 0xd4, 0xe2, 0xf0,
    0x8e, 0x9c, 0xaa, 0xb8, 0xc8, 0xda, 0xec, 0xfe, 0x80, 0x92, 0xa4, 0xb6,
    0x58, 0x4a, 0x7c, 0x6e, 0x10, 0x02, 0x34, 0x26, 0xfa, 0xe8, 0xde, 0xcc,
    0xb2, 0xa0, 0x96, 0x84, 0x6a, 0x78, 0x4e, 0x5c, 0x22, 0x30, 0x06, 0x14,
    0xac, 0xbe, 0x88, 0x9a, 0xe4, 0xf6, 0xc0, 0xd2, 0x3c, 0x2e, 0x18, 0x0a,
    0x74, 0x66, 0x50, 0x42, 0x9e, 0x8c, 0xba, 0xa8, 0xd6, 0xc4, 0xf2, 0xe0,
    0x0e, 0x1c, 0x2a, 0x38, 0x46, 0x54, 0x62, 0x70, 0x82, 0x90, 0xa6, 0xb4,
    0xca, 0xd8, 0xee, 0xfc, 0x12, 0x00, 0x36, 0x24, 0x5a, 0x48, 0x7e, 0x6c,
    0xb0, 0xa2, 0x94, 0x86, 0xf8, 0xea, 0xdc, 0xce, 0x20, 0x32, 0x04, 0x16,
    0x68, 0x7a, 0x4c, 0x5e, 0xe6, 0xf4, 0xc2, 0xd0, 0xae, 0xbc, 0x8a, 0x98,
    0x76, 0x64, 0x52, 0x40, 0x3e, 0x2c, 0x1a, 0x08, 0xd4, 0xc6, 0xf0, 0xe2,
    0x9c, 0x8e, 0xb8, 0xaa, 0x44, 0x56, 0x60, 0x72, 0x0c, 0x1e, 0x28, 0x3a,
    0x4a, 0x58, 0x6e, 0x7c, 0x02, 0x10, 0x26, 0x34, 0xda, 0xc8, 0xfe, 0xec,
    0x92, 0x80, 0xb6, 0xa4, 0x78, 0x6a, 0x5c, 0x4e, 0x30, 0x22, 0x14, 0x06,
    0xe8, 0xfa, 0xcc, 0xde, 0xa0, 0xb2, 0x84, 0x96, 0x2e, 0x3c, 0x0a, 0x18,
    0x66, 0x74, 0x42, 0x50, 0xbe, 0xac, 0x9a, 0x88, 0xf6, 0xe4, 0xd2, 0xc0,
    0x1c, 0x0e, 0x38, 0x2a, 0x54, 0x46, 0x70, 0x62, 0x8c, 0x9e, 0xa8, 0xba,
    0xc4, 0xd6, 0xe0, 0xf2,
};

uint8_t SD_RAMFUNC(sd_crc7)(const uint8_t *data, unsigned len) {
  uint8_t crc = 0;
  while (len--) crc = crc7_table[crc ^ *data++];
  return crc >> 1;
}

// CRC16-CCITT (x^16 + x^12 + x^5 + 1), initial value 0, for data blocks.
static const uint16_t SD_RAMDATA crc16_table[256] = {
    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0,
};

uint16_t SD_RAMFUNC(sd_crc16)(const uint8_t *data, unsigned len) {
  uint16_t crc = 0;
  while (len--)
    crc = (uint16_t)(crc << 8) ^ crc16_table[((crc >> 8) ^ *data++) & 0xFFu];
  return crc;
}

// Byte j of payload || CRC16 || 0xFF; the 0xFF holds the end bit (its other
// bits are never sent).
static inline __attribute__((always_inline)) uint32_t dat_byte(
    const uint8_t *data, unsigned len, uint16_t crc, unsigned j) {
  return j < len ? data[j] : j == len ? crc >> 8u : j == len + 1 ? crc & 0xFFu : 0xFFu;
}

void SD_RAMFUNC(sd_build_dat)(sd_dat_t *d, const uint8_t *data, unsigned len) {
  uint16_t crc = sd_crc16(data, len);
  unsigned nbytes = len + 3u;
  unsigned nwords = (nbytes + 3u) / 4u;
  uint32_t carry = 0;  // the start bit
  for (unsigned w = 0; w < nwords; w++) {
    uint32_t word = 0;
    for (unsigned k = 0; k < 4; k++) {
      unsigned j = w * 4u + k;
      uint32_t in = j < nbytes ? dat_byte(data, len, crc, j) : 0xFFu;
      word = word << 8 | carry << 7 | in >> 1;  // everything one bit later
      carry = in & 1u;
    }
    d->words[2 + w] = word;
  }
  d->words[0] = SD_DAT_DELAY_CLOCKS - 1u;
  d->words[1] = 8u * len + 18u - 1u;  // start + payload + CRC + end
  d->nwords = (uint8_t)(2u + nwords);
}

// ---------------------------------------------------------------------------
// Register contents

void sd_build_csd_v2(uint8_t csd[16], uint32_t c_size) {
  // CSD version 2.0 (SDHC/SDXC). Capacity = (C_SIZE + 1) * 512 KiB.
  csd[0] = 0x40;   // CSD_STRUCTURE = 1
  csd[1] = 0x0E;   // TAAC
  csd[2] = 0x00;   // NSAC
  csd[3] = 0x32;   // TRAN_SPEED: 25 MHz
  csd[4] = 0x5B;   // CCC = 0x5B5 ...
  csd[5] = 0x59;   // ... READ_BL_LEN = 9
  csd[6] = 0x00;
  csd[7] = (uint8_t)((c_size >> 16) & 0x3F);
  csd[8] = (uint8_t)(c_size >> 8);
  csd[9] = (uint8_t)c_size;
  csd[10] = 0x7F;  // ERASE_BLK_EN = 1, SECTOR_SIZE = 0x7F ...
  csd[11] = 0x80;
  csd[12] = 0x0A;  // R2W_FACTOR = 2, WRITE_BL_LEN = 9 ...
  csd[13] = 0x40;
  csd[14] = 0x00;
  csd[15] = 0x01;  // CRC7 filled in by sd_card_init()
}

void sd_build_cid(uint8_t cid[16], uint8_t mid, const char oid[2],
                  const char pnm[5], uint8_t prv, uint32_t psn,
                  unsigned year, unsigned month) {
  unsigned mdt = ((year - 2000u) & 0xFFu) << 4 | (month & 0xFu);
  cid[0] = mid;
  cid[1] = (uint8_t)oid[0];
  cid[2] = (uint8_t)oid[1];
  for (int i = 0; i < 5; i++) cid[3 + i] = (uint8_t)pnm[i];
  cid[8] = prv;
  cid[9] = (uint8_t)(psn >> 24);
  cid[10] = (uint8_t)(psn >> 16);
  cid[11] = (uint8_t)(psn >> 8);
  cid[12] = (uint8_t)psn;
  cid[13] = (uint8_t)(mdt >> 8);  // upper 4 bits reserved = 0
  cid[14] = (uint8_t)mdt;
  cid[15] = 0x01;
}

void sd_build_scr(uint8_t scr[8], uint32_t c_size, bool four_bit,
                  uint8_t cmd_support) {
  // SDHC up to C_SIZE 0xFF5F (32 GB), SDXC above.
  uint8_t security = c_size > 0xFF5Fu ? 4 : 3;
  scr[0] = 0x02;  // SCR_STRUCTURE 0, SD_SPEC 2 (with SD_SPEC3/4/X below)
  // SD_BUS_WIDTHS: bit 0 = 1 bit, bit 2 = 4 bits. Every real card has both;
  // this board can only send blocks in 1-bit mode (DAT1/DAT2 not wired).
  scr[1] = (uint8_t)(security << 4 | (four_bit ? 0x5 : 0x1));
  scr[2] = 0x84;  // SD_SPEC3 = 1, SD_SPEC4 = 1, SD_SPECX[3:2] = 0 ...
  // ... SD_SPECX[1:0] = 3 (version 7.x), CMD_SUPPORT in the low nibble
  scr[3] = (uint8_t)(0xC0 | (cmd_support & 0x0F));
  scr[4] = scr[5] = scr[6] = scr[7] = 0;
}

void sd_build_ssr(uint8_t ssr[64]) {
  // Values of a current microSDXC UHS-I card: C10, U3, V30, A1.
  for (int i = 0; i < 64; i++) ssr[i] = 0;
  ssr[0] = 0x00;   // DAT_BUS_WIDTH (patched per command), not secured
  ssr[8] = 0x04;   // SPEED_CLASS: class 10
  ssr[10] = 0x90;  // AU_SIZE = 9 (4 MB)
  ssr[14] = 0x39;  // UHS_SPEED_GRADE = 3, UHS_AU_SIZE = 9
  ssr[15] = 30;    // VIDEO_SPEED_CLASS: V30
  ssr[21] = 0x01;  // APP_PERF_CLASS: A1
}

// ---------------------------------------------------------------------------
// Frames

void SD_RAMFUNC(sd_decode)(uint32_t w0, uint32_t w1, sd_cmd_t *cmd) {
  uint8_t b[5] = {(uint8_t)(w0 >> 24), (uint8_t)(w0 >> 16), (uint8_t)(w0 >> 8),
                  (uint8_t)w0, (uint8_t)(w1 >> 8)};
  cmd->from_host = (w0 >> 30) & 1u;
  cmd->index = (w0 >> 24) & 0x3Fu;
  cmd->arg = (w0 << 8) | b[4];
  cmd->crc = (w1 >> 1) & 0x7Fu;
  cmd->end_ok = w1 & 1u;
  cmd->crc_ok = sd_crc7(b, 5) == cmd->crc;
}

// A 48-bit response: index (or 0x3F), 32-bit payload, CRC7 (or all ones).
static void SD_RAMFUNC(resp48)(sd_resp_t *resp, sd_resp_kind_t kind,
                               uint8_t index, uint32_t arg, bool with_crc) {
  uint8_t b[5] = {index & 0x3Fu, (uint8_t)(arg >> 24), (uint8_t)(arg >> 16),
                  (uint8_t)(arg >> 8), (uint8_t)arg};
  uint8_t last = with_crc ? (uint8_t)(sd_crc7(b, 5) << 1 | 1u) : 0xFFu;
  resp->words[0] = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
                   (uint32_t)b[2] << 8 | b[3];
  resp->words[1] = (uint32_t)b[4] << 24 | (uint32_t)last << 16;
  resp->nbits = 48;
  resp->kind = (uint8_t)kind;
  resp->arg = arg;
}

// R2: 0b00111111 then the 128-bit register, whose last byte already holds
// CRC7 and the end bit.
// Byte i of that 17-byte frame (zero padding past the end). Written out by
// hand rather than via a local array: a zero-filled array would make the
// compiler call memset, which lives in flash.
static inline __attribute__((always_inline)) uint32_t r2_byte(const uint8_t reg[16], int i) {
  return i == 0 ? 0x3Fu : i <= 16 ? reg[i - 1] : 0u;
}

static void SD_RAMFUNC(resp136)(sd_resp_t *resp, const uint8_t reg[16]) {
  for (int w = 0; w < 5; w++)
    resp->words[w] = r2_byte(reg, 4 * w) << 24 | r2_byte(reg, 4 * w + 1) << 16 |
                     r2_byte(reg, 4 * w + 2) << 8 | r2_byte(reg, 4 * w + 3);
  resp->nbits = 136;
  resp->kind = SD_RESP_R2;
  resp->arg = 0;
}

// ---------------------------------------------------------------------------
// State machine

void SD_RAMFUNC(sd_card_reset)(sd_card_t *card) {
  card->state = SD_STATE_IDLE;
  card->app_cmd = false;
  card->acmd41_count = 0;
  card->pending_err = 0;
  for (int g = 0; g < 6; g++) card->fg_current[g] = 0;
  card->bus_width = 1;
}

void sd_card_init(sd_card_t *card, sd_card_cfg_t *cfg) {
  cfg->cid[15] = (uint8_t)(sd_crc7(cfg->cid, 15) << 1 | 1u);
  cfg->csd[15] = (uint8_t)(sd_crc7(cfg->csd, 15) << 1 | 1u);
  card->cfg = cfg;
  card->pcie_acked = false;
  sd_card_reset(card);
}

static uint32_t SD_RAMFUNC(status_bits)(sd_card_t *card, sd_state_t state,
                                        bool app) {
  uint32_t st = card->pending_err | (uint32_t)state << SD_ST_STATE_SHIFT |
                SD_ST_READY_FOR_DATA;
  if (app) st |= SD_ST_APP_CMD;
  card->pending_err = 0;  // error bits are clear-on-read
  return st;
}

static void SD_RAMFUNC(r1)(sd_card_t *card, sd_resp_t *resp, uint8_t index,
                           sd_state_t state, bool app, sd_resp_kind_t kind) {
  resp48(resp, kind, index, status_bits(card, state, app), true);
}

static bool SD_RAMFUNC(addressed)(const sd_card_t *card, uint32_t arg) {
  return (arg >> 16) == card->cfg->rca;
}

// The data block of CMD6 / ACMD51 / ACMD13. On a 4-bit bus nothing can go
// out (DAT1/DAT2 are not wired), so it is skipped and the host times out.
static void SD_RAMFUNC(send_block)(const sd_card_t *card, sd_resp_t *resp,
                                   const uint8_t *data, uint8_t len) {
  if (card->bus_width == 4) {
    resp->dat_skipped = true;
    return;
  }
  resp->dat = data;
  resp->dat_len = len;
}

// ACMD41. Returns false when the command is not valid here.
static bool SD_RAMFUNC(acmd41)(sd_card_t *card, uint32_t arg,
                               sd_resp_t *resp) {
  const sd_card_cfg_t *cfg = card->cfg;
  uint32_t ocr = cfg->ocr & ~(SD_OCR_BUSY_N | SD_OCR_S18A);

  if (card->state != SD_STATE_IDLE && card->state != SD_STATE_READY)
    return false;
  if ((arg & 0x00FFFFFFu) == 0) {
    // Inquiry (empty voltage window): report the OCR, stay idle. CCS is only
    // valid once the busy bit says ready.
    resp48(resp, SD_RESP_R3, 0x3F, ocr & ~SD_OCR_CCS, false);
    return true;
  }
  if ((arg & cfg->ocr & SD_OCR_VDD_MASK) == 0) {
    card->state = SD_STATE_INA;  // voltage window mismatch
    return true;
  }
  if (card->state == SD_STATE_IDLE && card->acmd41_count < cfg->acmd41_busy) {
    card->acmd41_count++;
    resp48(resp, SD_RESP_R3, 0x3F, ocr & ~SD_OCR_CCS, false);
    return true;
  }
  card->state = SD_STATE_READY;
  // CCS is only meaningful to a host that set HCS.
  if (!(arg & SD_OCR_CCS)) ocr &= ~SD_OCR_CCS;
  resp48(resp, SD_RESP_R3, 0x3F, ocr | SD_OCR_BUSY_N, false);
  return true;
}

// CMD6 SWITCH_FUNC: builds the 512-bit status into card->cmd6_status and,
// in switch mode, selects the requested functions.
static void SD_RAMFUNC(cmd6)(sd_card_t *card, uint32_t arg) {
  const sd_card_cfg_t *cfg = card->cfg;
  uint8_t *st = card->cmd6_status;
  uint8_t result[6];
  bool error = false;

  for (int g = 0; g < 6; g++) {
    unsigned f = (arg >> (4 * g)) & 0xFu;
    if (f == 0xF)
      result[g] = card->fg_current[g];  // no change
    else if (cfg->fg_support[g] & (1u << f))
      result[g] = (uint8_t)f;
    else {
      result[g] = 0xF;
      error = true;
    }
  }
  // An unsupported function in any group cancels the whole switch.
  if ((arg & SD_CMD6_MODE_SWITCH) && !error)
    for (int g = 0; g < 6; g++) card->fg_current[g] = result[g];

  for (int i = 0; i < 64; i++) st[i] = 0;
  st[0] = (uint8_t)(cfg->max_current >> 8);
  st[1] = (uint8_t)cfg->max_current;
  for (int g = 0; g < 6; g++) {  // support bits, group 6 first
    st[2 + 2 * (5 - g)] = (uint8_t)(cfg->fg_support[g] >> 8);
    st[3 + 2 * (5 - g)] = (uint8_t)cfg->fg_support[g];
  }
  st[14] = (uint8_t)(result[5] << 4 | result[4]);
  st[15] = (uint8_t)(result[3] << 4 | result[2]);
  st[16] = (uint8_t)(result[1] << 4 | result[0]);
  st[17] = 0x01;  // data structure version 1: busy status (all 0) follows
}

// CMD8. Returns false when the command is not valid here.
static bool SD_RAMFUNC(cmd8)(sd_card_t *card, uint32_t arg, sd_resp_t *resp,
                             uint8_t *flags) {
  const sd_card_cfg_t *cfg = card->cfg;
  if (card->state != SD_STATE_IDLE) return false;
  // A voltage we cannot take: a real card stays silent.
  if ((arg & SD_CMD8_VHS_MASK) != SD_CMD8_VHS_27_36) return true;

  uint32_t r7 = arg & (SD_CMD8_VHS_MASK | 0xFFu);
  if (cfg->pcie_ack && (arg & SD_CMD8_PCIE_AVAIL)) {
    r7 |= SD_CMD8_PCIE_AVAIL;
    if (cfg->pcie_1v2 && (arg & SD_CMD8_PCIE_1V2)) r7 |= SD_CMD8_PCIE_1V2;
    card->pcie_acked = true;
    *flags |= SD_LOGF_PCIE_ACK;
  }
  resp48(resp, SD_RESP_R7, 8, r7, true);
  return true;
}

uint8_t SD_RAMFUNC(sd_card_handle)(sd_card_t *card, const sd_cmd_t *cmd,
                                   sd_resp_t *resp) {
  const sd_card_cfg_t *cfg = card->cfg;
  sd_state_t st = card->state;
  uint8_t flags = cmd->crc_ok ? SD_LOGF_CRC_OK : 0;
  bool ok = true;

  resp->nbits = 0;
  resp->kind = SD_RESP_NONE;
  resp->arg = 0;
  resp->dat_len = 0;
  resp->dat_skipped = false;

  if (!cmd->from_host || !cmd->end_ok) return flags | SD_LOGF_BAD_FRAME;
  if (cfg->check_crc && !cmd->crc_ok) {
    card->pending_err |= SD_ST_COM_CRC_ERROR;
    card->app_cmd = false;
    return flags;
  }

  // CMD0 also brings us back from the inactive state. A real card needs a
  // power cycle for that, but this board may be powered over USB.
  if (cmd->index == 0) {
    sd_card_reset(card);
    return flags;
  }
  if (st == SD_STATE_INA) return flags;

  bool app = card->app_cmd;
  card->app_cmd = false;
  if (app) flags |= SD_LOGF_APP;

  // The identification beyond CMD8 can be switched off for experiments; the
  // card then behaves as if it only knew CMD0 and CMD8.
  bool sd = cfg->sd_mode;

  if (app && cmd->index == 41) {
    ok = sd && acmd41(card, cmd->arg, resp);
  } else if (app && cmd->index == 42) {
    ok = sd && st == SD_STATE_TRAN;
    if (ok) r1(card, resp, 42, st, true, SD_RESP_R1);
  } else if (app && cmd->index == 6) {
    // SET_BUS_WIDTH: 00 = 1 bit, 10 = 4 bits (only if the SCR claims it).
    unsigned w = cmd->arg & 3u;
    ok = sd && st == SD_STATE_TRAN && (w == 0 || (w == 2 && cfg->scr_4bit));
    if (ok) {
      r1(card, resp, 6, st, true, SD_RESP_R1);
      card->bus_width = w == 2 ? 4 : 1;
    }
  } else if (app && cmd->index == 51) {
    // SEND_SCR: R1, then the register on DAT0.
    ok = sd && st == SD_STATE_TRAN;
    if (ok) {
      r1(card, resp, 51, st, true, SD_RESP_R1);
      send_block(card, resp, cfg->scr, 8);
    }
  } else if (app && cmd->index == 13) {
    // SD_STATUS, with the current bus width (10 = 4 bits) in bits 511:510.
    ok = sd && st == SD_STATE_TRAN;
    if (ok) {
      r1(card, resp, 13, st, true, SD_RESP_R1);
      for (int i = 0; i < 64; i++) card->ssr_now[i] = cfg->ssr[i];
      card->ssr_now[0] = (uint8_t)((card->ssr_now[0] & 0x3Fu) |
                                   (card->bus_width == 4 ? 0x80u : 0u));
      send_block(card, resp, card->ssr_now, 64);
    }
  } else if (app && (cmd->index == 22 || cmd->index == 23)) {
    ok = false;  // write-related
  } else {
    // Normal commands; an undefined ACMD is handled as the plain command.
    switch (cmd->index) {
      case 8:
        ok = cmd8(card, cmd->arg, resp, &flags);
        break;
      case 55:
        if (!sd) {
          ok = false;
        } else if (st == SD_STATE_IDLE || st == SD_STATE_READY ||
                   st == SD_STATE_IDENT || addressed(card, cmd->arg)) {
          card->app_cmd = true;
          r1(card, resp, 55, st, true, SD_RESP_R1);
        }
        // An addressed command for another RCA: silently ignored.
        break;
      case 2:
        ok = sd && st == SD_STATE_READY;
        if (ok) {
          resp136(resp, cfg->cid);
          card->state = SD_STATE_IDENT;
        }
        break;
      case 3:
        ok = sd && (st == SD_STATE_IDENT || st == SD_STATE_STBY);
        if (ok) {
          uint32_t full = status_bits(card, st, false);
          uint32_t r6 = (uint32_t)cfg->rca << 16 |
                        ((full >> 8) & 0xC000u) |  // bits 23,22 -> 15,14
                        ((full >> 6) & 0x2000u) |  // bit 19 -> 13
                        (full & 0x1FFFu);
          resp48(resp, SD_RESP_R6, 3, r6, true);
          card->state = SD_STATE_STBY;
        }
        break;
      case 9:
      case 10:
        ok = sd && st == SD_STATE_STBY;
        if (ok && addressed(card, cmd->arg))
          resp136(resp, cmd->index == 9 ? cfg->csd : cfg->cid);
        break;
      case 7:
        ok = sd && (st == SD_STATE_STBY || st == SD_STATE_TRAN ||
                    st == SD_STATE_DATA || st == SD_STATE_DIS);
        if (!ok) break;
        if (addressed(card, cmd->arg)) {
          r1(card, resp, 7, st, false, SD_RESP_R1B);
          if (st == SD_STATE_STBY) card->state = SD_STATE_TRAN;
        } else if (st == SD_STATE_TRAN || st == SD_STATE_DATA) {
          card->state = SD_STATE_STBY;  // deselected, no response
        }
        break;
      case 13:
        ok = sd && st >= SD_STATE_STBY && st <= SD_STATE_DIS;
        if (ok && addressed(card, cmd->arg))
          r1(card, resp, 13, st, app, SD_RESP_R1);
        break;
      case 15:
        ok = sd && st >= SD_STATE_STBY && st <= SD_STATE_DIS;
        if (ok && addressed(card, cmd->arg)) card->state = SD_STATE_INA;
        break;
      case 6:  // SWITCH_FUNC: R1, then the 512-bit status on DAT0
        ok = sd && st == SD_STATE_TRAN;
        if (ok) {
          r1(card, resp, 6, st, false, SD_RESP_R1);
          cmd6(card, cmd->arg);
          send_block(card, resp, card->cmd6_status, 64);
        }
        break;
      case 20:  // SPEED_CLASS_CONTROL, if the SCR claims it: R1b
        ok = sd && st == SD_STATE_TRAN && (cfg->cmd_support & 1u);
        if (ok) r1(card, resp, 20, st, false, SD_RESP_R1B);
        break;
      case 23:  // SET_BLOCK_COUNT, if the SCR claims it: R1
        ok = sd && st == SD_STATE_TRAN && (cfg->cmd_support & 2u);
        if (ok) r1(card, resp, 23, st, false, SD_RESP_R1);
        break;
      case 4:  // SET_DSR: no response
        ok = sd && st == SD_STATE_STBY;
        break;
      case 16:  // SET_BLOCKLEN: fine, as long as it is 512
        ok = sd && st == SD_STATE_TRAN;
        if (ok) {
          if (cmd->arg != 512) card->pending_err |= 1u << 29;  // BLOCK_LEN_ERROR
          r1(card, resp, 16, st, false, SD_RESP_R1);
        }
        break;
      default:
        // CMD1/CMD5 (not MMC, not SDIO), CMD11 (no 1.8 V signalling), and
        // everything that needs the DAT lines.
        ok = false;
        break;
    }
  }

  if (!ok) {
    card->pending_err |= SD_ST_ILLEGAL_COMMAND;
    flags |= SD_LOGF_ILLEGAL;
  }
  if (resp->nbits) flags |= SD_LOGF_RESPONDED;
  if (resp->dat_len) flags |= SD_LOGF_DATA;
  return flags;
}
