// Host-side test of src/sd_proto.c: build with any C compiler, e.g.
//   gcc -DSD_PROTO_HOST_TEST -I../src -o test_sd_proto test_sd_proto.c ../src/sd_proto.c
// and run; it exits non-zero on the first failure.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sd_proto.h"

static int failures;

#define CHECK(cond)                                                 \
  do {                                                              \
    if (!(cond)) {                                                  \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
      failures++;                                                   \
    }                                                               \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    unsigned long _a = (unsigned long)(a), _b = (unsigned long)(b);                                 \
    if (_a != _b) {                                                        \
      printf("FAIL %s:%d: %s = 0x%lx, expected 0x%lx\n", __FILE__,       \
             __LINE__, #a, _a, _b);                                        \
      failures++;                                                          \
    }                                                                      \
  } while (0)

// The two words the PIO receiver pushes for a host command: bits 47..16 and
// bits 15..0.
static void host_frame(uint8_t index, uint32_t arg, uint32_t *w0, uint32_t *w1,
                       int corrupt_crc) {
  uint8_t b[5] = {(uint8_t)(0x40 | index), (uint8_t)(arg >> 24),
                  (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg};
  uint8_t crc = sd_crc7(b, 5) ^ (corrupt_crc ? 1 : 0);
  *w0 = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | (uint32_t)b[2] << 8 | b[3];
  *w1 = (uint32_t)b[4] << 8 | (uint32_t)(crc << 1 | 1);
}

static sd_card_cfg_t cfg;
static sd_card_t card;
static sd_resp_t resp;

static uint8_t send(uint8_t index, uint32_t arg) {
  uint32_t w0, w1;
  sd_cmd_t cmd;
  host_frame(index, arg, &w0, &w1, 0);
  sd_decode(w0, w1, &cmd);
  CHECK(cmd.from_host && cmd.end_ok && cmd.crc_ok);
  CHECK_EQ(cmd.index, index);
  CHECK_EQ(cmd.arg, arg);
  return sd_card_handle(&card, &cmd, &resp);
}

// Checks a 48-bit response: index field, payload, and CRC7 (or 0xFF for R3).
static void check_resp48(uint8_t index, uint32_t payload, int has_crc) {
  CHECK_EQ(resp.nbits, 48);
  uint8_t b[6] = {(uint8_t)(resp.words[0] >> 24), (uint8_t)(resp.words[0] >> 16),
                  (uint8_t)(resp.words[0] >> 8), (uint8_t)resp.words[0],
                  (uint8_t)(resp.words[1] >> 24), (uint8_t)(resp.words[1] >> 16)};
  CHECK_EQ(b[0] & 0xC0, 0);  // start bit 0, transmission bit 0 (card)
  CHECK_EQ(b[0] & 0x3F, index);
  CHECK_EQ((uint32_t)b[1] << 24 | b[2] << 16 | b[3] << 8 | b[4], payload);
  if (has_crc)
    CHECK_EQ(b[5], sd_crc7(b, 5) << 1 | 1);
  else
    CHECK_EQ(b[5], 0xFF);
  CHECK_EQ(resp.words[1] & 0xFFFF, 0);
}

static void reset_cfg(void) {
  memset(&cfg, 0, sizeof cfg);
  cfg.pcie_ack = true;
  cfg.pcie_1v2 = false;
  cfg.sd_mode = true;
  cfg.check_crc = true;
  cfg.acmd41_busy = 2;
  cfg.rca = 0x1234;
  cfg.ocr = SD_OCR_VDD_MASK | SD_OCR_CCS;
  sd_build_cid(cfg.cid, 0x00, "KG", "KGEXP", 0x10, 0x01020304, 2026, 11);
  sd_build_csd_v2(cfg.csd, 0x3B37F);
  // The 1-bit-only, no-extras card; tests that want more turn it on.
  cfg.scr_4bit = false;
  cfg.cmd_support = 0;
  sd_build_scr(cfg.scr, 0x3B37F, cfg.scr_4bit, cfg.cmd_support);
  sd_build_ssr(cfg.ssr);
  for (int g = 0; g < 6; g++) cfg.fg_support[g] = 0x8001;
  cfg.max_current = 100;
  sd_card_init(&card, &cfg);
}

// Brings the card to the transfer state.
static void to_tran(void) {
  send(0, 0);
  send(8, 0x11AA);
  for (int i = 0; i <= cfg.acmd41_busy; i++) {
    send(55, 0);
    send(41, 0x40FF8000);
  }
  send(2, 0);
  send(3, 0);
  send(7, (uint32_t)cfg.rca << 16);
  CHECK_EQ(card.state, SD_STATE_TRAN);
}

// Unpacks a DAT0 frame the way the host sees it: returns the bit at `i`.
static int dat_bit(const sd_dat_t *d, unsigned i) {
  return (d->words[2 + i / 32] >> (31 - i % 32)) & 1;
}

// Checks start bit, payload, CRC16 and end bit of a framed block.
static void check_dat(const sd_dat_t *d, const uint8_t *payload, unsigned len) {
  unsigned nbits = 8 * len + 18;
  CHECK_EQ(d->words[0], SD_DAT_DELAY_CLOCKS - 1);
  CHECK_EQ(d->words[1], nbits - 1);
  CHECK_EQ(d->nwords, 2 + (len + 3 + 3) / 4);
  CHECK_EQ(dat_bit(d, 0), 0);
  for (unsigned i = 0; i < len * 8; i++)
    CHECK_EQ(dat_bit(d, 1 + i), (payload[i / 8] >> (7 - i % 8)) & 1);
  uint16_t crc = sd_crc16(payload, len);
  for (unsigned i = 0; i < 16; i++)
    CHECK_EQ(dat_bit(d, 1 + 8 * len + i), (crc >> (15 - i)) & 1);
  CHECK_EQ(dat_bit(d, nbits - 1), 1);
}

static void test_crc7(void) {
  // Well-known values from the SD spec and SPI-mode init sequences.
  uint8_t cmd0[5] = {0x40, 0, 0, 0, 0};
  uint8_t cmd8[5] = {0x48, 0, 0, 0x01, 0xAA};
  uint8_t cmd17[5] = {0x51, 0, 0, 0, 0};
  uint8_t r7[5] = {0x08, 0, 0, 0x01, 0xAA};
  uint8_t acmd41[5] = {0x69, 0x40, 0, 0, 0};
  CHECK_EQ(sd_crc7(cmd0, 5) << 1 | 1, 0x95);
  CHECK_EQ(sd_crc7(cmd8, 5) << 1 | 1, 0x87);
  CHECK_EQ(sd_crc7(cmd17, 5) << 1 | 1, 0x55);
  CHECK_EQ(sd_crc7(acmd41, 5) << 1 | 1, 0x77);
  CHECK_EQ(sd_crc7(r7, 5) << 1 | 1, 0x13);
}

static void test_crc16(void) {
  // SD spec example: 512 bytes of 0xFF.
  static uint8_t ff[512];
  memset(ff, 0xFF, sizeof ff);
  CHECK_EQ(sd_crc16(ff, 512), 0x7FA1);
}

static void test_dat_framing(void) {
  uint8_t data[64];
  sd_dat_t d;
  for (unsigned i = 0; i < 64; i++) data[i] = (uint8_t)(i * 37 + 5);
  sd_build_dat(&d, data, 64);
  check_dat(&d, data, 64);
  sd_build_dat(&d, data, 8);
  check_dat(&d, data, 8);
}

static void test_scr(void) {
  uint8_t scr[8];
  sd_build_scr(scr, 0x3B37F, false, 0);  // SDXC, 1 bit only
  const uint8_t sdxc[8] = {0x02, 0x41, 0x84, 0xC0, 0, 0, 0, 0};
  CHECK(memcmp(scr, sdxc, 8) == 0);
  sd_build_scr(scr, 0xEDC8, false, 0);  // SDHC
  CHECK_EQ(scr[1], 0x31);
  // Like a real SDXC card: 1 and 4 bits, CMD20 + CMD23.
  sd_build_scr(scr, 0x3B37F, true, 0x3);
  const uint8_t real[8] = {0x02, 0x45, 0x84, 0xC3, 0, 0, 0, 0};
  CHECK(memcmp(scr, real, 8) == 0);
}

static void test_ssr(void) {
  uint8_t ssr[64];
  sd_build_ssr(ssr);
  CHECK_EQ(ssr[8], 0x04);    // C10
  CHECK_EQ(ssr[14] >> 4, 3); // U3
  CHECK_EQ(ssr[15], 30);     // V30
  CHECK_EQ(ssr[21] & 0xF, 1);  // A1
}

static void test_data_commands(void) {
  reset_cfg();
  to_tran();
  sd_dat_t d;

  // ACMD51: R1 (APP_CMD set), then the SCR on DAT0.
  send(55, (uint32_t)cfg.rca << 16);
  uint8_t f = send(51, 0);
  CHECK(f & SD_LOGF_DATA);
  check_resp48(51, SD_STATE_TRAN << SD_ST_STATE_SHIFT | SD_ST_READY_FOR_DATA |
                       SD_ST_APP_CMD, 1);
  CHECK_EQ(resp.dat_len, 8);
  CHECK(memcmp(resp.dat, cfg.scr, 8) == 0);
  sd_build_dat(&d, resp.dat, resp.dat_len);
  check_dat(&d, cfg.scr, 8);

  // ACMD13: SD status.
  send(55, (uint32_t)cfg.rca << 16);
  send(13, 0);
  CHECK_EQ(resp.dat_len, 64);
  CHECK(memcmp(resp.dat, cfg.ssr, 64) == 0);  // 1 bit: DAT_BUS_WIDTH 00

  // CMD6 mode 0, all groups "no change": current functions (0), version 1.
  f = send(6, 0x00FFFFFF);
  CHECK(f & SD_LOGF_DATA);
  check_resp48(6, SD_STATE_TRAN << SD_ST_STATE_SHIFT | SD_ST_READY_FOR_DATA, 1);
  CHECK_EQ(resp.dat_len, 64);
  const uint8_t *st = resp.dat;
  CHECK_EQ(st[0] << 8 | st[1], 100);
  for (int g = 0; g < 6; g++) CHECK_EQ(st[2 + 2 * g] << 8 | st[3 + 2 * g], 0x8001);
  CHECK_EQ(st[14], 0x00);
  CHECK_EQ(st[15], 0x00);
  CHECK_EQ(st[16], 0x00);
  CHECK_EQ(st[17], 0x01);
  for (int i = 18; i < 64; i++) CHECK_EQ(st[i], 0);
  sd_build_dat(&d, resp.dat, resp.dat_len);
  check_dat(&d, st, 64);

  // CMD6 mode 0 asking for High Speed (FG1 function 1): not supported.
  send(6, 0x00FFFFF1);
  CHECK_EQ(resp.dat[16], 0x0F);

  // With High Speed supported: check, then switch.
  cfg.fg_support[0] = 0x8003;
  send(6, 0x00FFFFF1);
  CHECK_EQ(resp.dat[16], 0x01);
  CHECK_EQ(card.fg_current[0], 0);  // mode 0 does not switch
  send(6, 0x80FFFFF1);
  CHECK_EQ(resp.dat[16], 0x01);
  CHECK_EQ(card.fg_current[0], 1);
  send(6, 0x00FFFFFF);
  CHECK_EQ(resp.dat[16], 0x01);  // reported as current

  // A switch with one unsupported group changes nothing.
  send(6, 0x80FFFF20);  // FG1 -> 0, FG2 -> 2 (unsupported)
  CHECK_EQ(resp.dat[16], 0xF0);
  CHECK_EQ(card.fg_current[0], 1);

  // CMD6 outside the transfer state is illegal.
  reset_cfg();
  send(0, 0);
  f = send(6, 0x00FFFFFF);
  CHECK(f & SD_LOGF_ILLEGAL);
  CHECK_EQ(resp.nbits, 0);
  CHECK_EQ(resp.dat_len, 0);
}

static void test_bus_width(void) {
  reset_cfg();
  to_tran();
  send(55, (uint32_t)cfg.rca << 16);
  send(6, 0);  // 1 bit: fine
  check_resp48(6, SD_STATE_TRAN << SD_ST_STATE_SHIFT | SD_ST_READY_FOR_DATA |
                      SD_ST_APP_CMD, 1);
  send(55, (uint32_t)cfg.rca << 16);
  uint8_t f = send(6, 2);  // 4 bit, but the SCR does not claim it
  CHECK(f & SD_LOGF_ILLEGAL);
  CHECK_EQ(resp.nbits, 0);
  CHECK_EQ(card.bus_width, 1);

  // Claimed in the SCR: accepted, and data blocks can then not be sent.
  reset_cfg();
  cfg.scr_4bit = true;
  to_tran();
  send(55, (uint32_t)cfg.rca << 16);
  f = send(6, 2);
  CHECK(!(f & SD_LOGF_ILLEGAL));
  check_resp48(6, SD_STATE_TRAN << SD_ST_STATE_SHIFT | SD_ST_READY_FOR_DATA |
                      SD_ST_APP_CMD, 1);
  CHECK_EQ(card.bus_width, 4);
  f = send(6, 0x00FFFFFF);  // CMD6: R1, no block
  CHECK_EQ(resp.nbits, 48);
  CHECK_EQ(resp.dat_len, 0);
  CHECK(resp.dat_skipped);
  CHECK(!(f & SD_LOGF_DATA));
  // SD status would report 4 bits (10) in DAT_BUS_WIDTH.
  send(55, (uint32_t)cfg.rca << 16);
  send(13, 0);
  CHECK(resp.dat_skipped);
  CHECK_EQ(card.ssr_now[0] >> 6, 2);
  // Back to 1 bit, blocks go out again.
  send(55, (uint32_t)cfg.rca << 16);
  send(6, 0);
  CHECK_EQ(card.bus_width, 1);
  send(6, 0x00FFFFFF);
  CHECK_EQ(resp.dat_len, 64);
  CHECK(!resp.dat_skipped);
  // CMD0 resets the width.
  send(55, (uint32_t)cfg.rca << 16);
  send(6, 2);
  send(0, 0);
  CHECK_EQ(card.bus_width, 1);
}

static void test_cmd20_cmd23(void) {
  reset_cfg();
  to_tran();
  uint8_t f = send(23, 8);  // not claimed in the SCR: illegal
  CHECK(f & SD_LOGF_ILLEGAL);
  reset_cfg();
  cfg.cmd_support = 0x3;
  to_tran();
  send(23, 8);
  check_resp48(23, SD_STATE_TRAN << SD_ST_STATE_SHIFT | SD_ST_READY_FOR_DATA, 1);
  send(20, 0);
  CHECK_EQ(resp.kind, SD_RESP_R1B);
}

static void test_csd_layout(void) {
  // Matches a typical SDHC CSD: 400E00325B59 0000xxxx 7F800A4000xx
  uint8_t csd[16];
  sd_build_csd_v2(csd, 0x00EDC8);
  const uint8_t expect[15] = {0x40, 0x0E, 0x00, 0x32, 0x5B, 0x59, 0x00, 0x00,
                              0xED, 0xC8, 0x7F, 0x80, 0x0A, 0x40, 0x00};
  CHECK(memcmp(csd, expect, 15) == 0);

  // The default: a 512 GB SSD (512,110,190,592 bytes) rounded down to
  // 512 KiB: C_SIZE 976771 = 0x0EE783 in bits 69:48.
  sd_build_csd_v2(csd, 976771);
  CHECK_EQ(csd[7], 0x0E);
  CHECK_EQ(csd[8], 0xE7);
  CHECK_EQ(csd[9], 0x83);
  uint32_t c_size = (uint32_t)(csd[7] & 0x3F) << 16 | csd[8] << 8 | csd[9];
  unsigned long long bytes = (unsigned long long)(c_size + 1) * 512 * 1024;
  CHECK_EQ(bytes == 512109838336ULL, 1);
  CHECK_EQ(bytes <= 512110190592ULL && 512110190592ULL - bytes < 512 * 1024, 1);
  uint8_t scr[8];
  sd_build_scr(scr, 976771, false, 0);
  CHECK_EQ(scr[1] >> 4, 4);  // SDXC
}

static void test_express_handshake(void) {
  reset_cfg();
  CHECK_EQ(send(0, 0) & SD_LOGF_RESPONDED, 0);
  CHECK_EQ(resp.nbits, 0);

  // Linux-style probe: PCIe availability + 1.2 V, VHS 2.7-3.6 V, 0xAA.
  uint8_t f = send(8, SD_CMD8_PCIE_AVAIL | SD_CMD8_PCIE_1V2 | 0x1AA);
  CHECK(f & SD_LOGF_PCIE_ACK);
  CHECK(f & SD_LOGF_RESPONDED);
  CHECK_EQ(resp.kind, SD_RESP_R7);
  check_resp48(8, SD_CMD8_PCIE_AVAIL | 0x1AA, 1);  // 1.2 V not claimed
  CHECK(card.pcie_acked);

  // With 1.2 V claimed.
  cfg.pcie_1v2 = true;
  send(0, 0);
  send(8, SD_CMD8_PCIE_AVAIL | SD_CMD8_PCIE_1V2 | 0x1AA);
  check_resp48(8, SD_CMD8_PCIE_AVAIL | SD_CMD8_PCIE_1V2 | 0x1AA, 1);

  // A host that does not ask about PCIe gets a plain R7.
  cfg.pcie_1v2 = false;
  send(0, 0);
  f = send(8, 0x1AA);
  CHECK(!(f & SD_LOGF_PCIE_ACK));
  check_resp48(8, 0x1AA, 1);

  // PCIe acknowledgement switched off.
  cfg.pcie_ack = false;
  send(0, 0);
  send(8, SD_CMD8_PCIE_AVAIL | 0x1AA);
  check_resp48(8, 0x1AA, 1);
  cfg.pcie_ack = true;

  // Unsupported voltage: silence.
  send(0, 0);
  send(8, 0x2AA);
  CHECK_EQ(resp.nbits, 0);
}

static void test_identification(void) {
  reset_cfg();
  send(0, 0);
  send(8, 0x11AA);

  // CMD55 in idle: R1, state idle, APP_CMD.
  send(55, 0);
  check_resp48(55, SD_ST_READY_FOR_DATA | SD_ST_APP_CMD, 1);

  // ACMD41 inquiry.
  send(41, 0);
  check_resp48(0x3F, SD_OCR_VDD_MASK, 0);
  CHECK_EQ(card.state, SD_STATE_IDLE);

  // Two busy answers, then ready with CCS.
  for (int i = 0; i < 2; i++) {
    send(55, 0);
    uint8_t f = send(41, 0x40FF8000);
    CHECK(f & SD_LOGF_APP);
    check_resp48(0x3F, SD_OCR_VDD_MASK, 0);
  }
  send(55, 0);
  send(41, 0x40FF8000);
  check_resp48(0x3F, SD_OCR_BUSY_N | SD_OCR_CCS | SD_OCR_VDD_MASK, 0);
  CHECK_EQ(card.state, SD_STATE_READY);

  // CMD2: CID.
  send(2, 0);
  CHECK_EQ(resp.nbits, 136);
  CHECK_EQ(resp.words[0] >> 24, 0x3F);
  CHECK_EQ((resp.words[0] >> 16) & 0xFF, cfg.cid[0]);
  CHECK_EQ((resp.words[4] >> 24), cfg.cid[15]);
  CHECK_EQ(cfg.cid[15] & 1, 1);
  CHECK_EQ(card.state, SD_STATE_IDENT);

  // CMD3: R6 with our RCA, status = ident | READY_FOR_DATA.
  send(3, 0);
  check_resp48(3, 0x12340500, 1);
  CHECK_EQ(card.state, SD_STATE_STBY);

  // CMD9 for another RCA: ignored. For ours: CSD.
  send(9, 0x43210000);
  CHECK_EQ(resp.nbits, 0);
  send(9, 0x12340000);
  CHECK_EQ(resp.nbits, 136);
  CHECK_EQ((resp.words[0] >> 16) & 0xFF, 0x40);

  // CMD7 select: R1b reporting stby, then tran.
  send(7, 0x12340000);
  CHECK_EQ(resp.kind, SD_RESP_R1B);
  check_resp48(7, SD_STATE_STBY << SD_ST_STATE_SHIFT | SD_ST_READY_FOR_DATA, 1);
  CHECK_EQ(card.state, SD_STATE_TRAN);

  send(13, 0x12340000);
  check_resp48(13, SD_STATE_TRAN << SD_ST_STATE_SHIFT | SD_ST_READY_FOR_DATA, 1);

  // Unsupported data command: silence, ILLEGAL_COMMAND in the next status.
  uint8_t f = send(17, 0);
  CHECK(f & SD_LOGF_ILLEGAL);
  CHECK_EQ(resp.nbits, 0);
  send(13, 0x12340000);
  check_resp48(13, SD_ST_ILLEGAL_COMMAND | SD_STATE_TRAN << SD_ST_STATE_SHIFT |
                       SD_ST_READY_FOR_DATA, 1);
  // ... and only once.
  send(13, 0x12340000);
  check_resp48(13, SD_STATE_TRAN << SD_ST_STATE_SHIFT | SD_ST_READY_FOR_DATA, 1);

  // Deselect.
  send(7, 0);
  CHECK_EQ(resp.nbits, 0);
  CHECK_EQ(card.state, SD_STATE_STBY);
}

static void test_crc_error(void) {
  reset_cfg();
  uint32_t w0, w1;
  sd_cmd_t cmd;
  host_frame(8, 0x11AA, &w0, &w1, 1);
  sd_decode(w0, w1, &cmd);
  CHECK(!cmd.crc_ok);
  sd_card_handle(&card, &cmd, &resp);
  CHECK_EQ(resp.nbits, 0);
  send(55, 0);
  check_resp48(55, SD_ST_COM_CRC_ERROR | SD_ST_READY_FOR_DATA | SD_ST_APP_CMD,
               1);

  // With CRC checking off the command goes through.
  reset_cfg();
  cfg.check_crc = false;
  sd_decode(w0, w1, &cmd);
  sd_card_handle(&card, &cmd, &resp);
  check_resp48(8, 0x11AA, 1);
}

static void test_card_response_ignored(void) {
  reset_cfg();
  // What the receiver would see if it caught a card's R7: transmission bit 0.
  uint32_t w0 = 0x08000011u, w1 = 0xAA13u;
  sd_cmd_t cmd;
  sd_decode(w0, w1, &cmd);
  CHECK(!cmd.from_host);
  uint8_t f = sd_card_handle(&card, &cmd, &resp);
  CHECK(f & SD_LOGF_BAD_FRAME);
  CHECK_EQ(resp.nbits, 0);
}

static void test_cmd8_only_mode(void) {
  reset_cfg();
  cfg.sd_mode = false;
  send(0, 0);
  send(8, 0x11AA);
  check_resp48(8, 0x11AA, 1);
  send(55, 0);
  CHECK_EQ(resp.nbits, 0);
  send(41, 0x40FF8000);
  CHECK_EQ(resp.nbits, 0);
}

static void test_voltage_mismatch(void) {
  reset_cfg();
  send(0, 0);
  send(8, 0x1AA);
  send(55, 0);
  send(41, 0x40000080);  // only the low-voltage-range bit
  CHECK_EQ(resp.nbits, 0);
  CHECK_EQ(card.state, SD_STATE_INA);
  send(55, 0);
  CHECK_EQ(resp.nbits, 0);
  send(0, 0);  // recovers
  CHECK_EQ(card.state, SD_STATE_IDLE);
}

int main(void) {
  test_crc7();
  test_crc16();
  test_dat_framing();
  test_scr();
  test_csd_layout();
  test_express_handshake();
  test_identification();
  test_crc_error();
  test_card_response_ignored();
  test_cmd8_only_mode();
  test_voltage_mismatch();
  test_data_commands();
  test_bus_width();
  test_ssr();
  test_cmd20_cmd23();
  if (failures) {
    printf("%d failure(s)\n", failures);
    return 1;
  }
  printf("all sd_proto tests passed\n");
  return 0;
}
