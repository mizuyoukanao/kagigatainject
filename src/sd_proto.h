// SD-mode card-side protocol: frame decoding, CRC7, response building and the
// card state machine. Pure logic with no hardware access, so the same file is
// compiled into the firmware (where it runs on core1 from RAM) and into the
// host-side unit test in test/.
//
// What a host sees from us is a microSD card that answers CMD8 with the PCIe
// availability bit set, i.e. an SD Express card. A host that wants PCIe then
// turns on VDD2, which on the adapter board moves the card pins over to the
// M.2 SSD; a host that does not continues the normal SD initialisation
// (ACMD41, CMD2, CMD3, CMD7, ACMD51, CMD6, ...), which we also answer so that
// it can get as far as it needs to. Commands with a data block (CMD6, ACMD51,
// ACMD13) send it on DAT0, the only data line this board has. The registers
// can claim what a real card would (4-bit bus, High Speed), but a block asked
// for after a switch to 4 bits cannot be sent: DAT1/DAT2 are not wired.
#ifndef SD_PROTO_H
#define SD_PROTO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef SD_PROTO_HOST_TEST
#define SD_RAMFUNC(name) name
#define SD_RAMDATA
#else
#include "pico.h"
// Everything on the command path runs on core1 while core0 may be erasing
// flash, so it must never be fetched through XIP.
#define SD_RAMFUNC(name) __not_in_flash_func(name)
// Same for tables: without this GCC moves a never-written array to flash.
#define SD_RAMDATA __not_in_flash("sd_proto")
#endif

// CMD8 argument / R7 bits (SD 7.x). Linux names these the same way in
// mmc_send_if_cond_pcie().
#define SD_CMD8_PCIE_AVAIL (1u << 12)
#define SD_CMD8_PCIE_1V2 (1u << 13)
#define SD_CMD8_VHS_MASK (0xFu << 8)
#define SD_CMD8_VHS_27_36 (0x1u << 8)

// OCR bits.
#define SD_OCR_BUSY_N (1u << 31)  // 1 = power-up finished
#define SD_OCR_CCS (1u << 30)     // SDHC/SDXC
#define SD_OCR_UHS2 (1u << 29)
#define SD_OCR_S18A (1u << 24)
#define SD_OCR_VDD_MASK 0x00FF8000u  // 2.7-3.6 V

// CMD6 argument.
#define SD_CMD6_MODE_SWITCH (1u << 31)

// Card status bits (R1).
#define SD_ST_OUT_OF_RANGE (1u << 31)
#define SD_ST_COM_CRC_ERROR (1u << 23)
#define SD_ST_ILLEGAL_COMMAND (1u << 22)
#define SD_ST_READY_FOR_DATA (1u << 8)
#define SD_ST_APP_CMD (1u << 5)
#define SD_ST_STATE_SHIFT 9

typedef enum {
  SD_STATE_IDLE = 0,
  SD_STATE_READY = 1,
  SD_STATE_IDENT = 2,
  SD_STATE_STBY = 3,
  SD_STATE_TRAN = 4,
  SD_STATE_DATA = 5,
  SD_STATE_RCV = 6,
  SD_STATE_PRG = 7,
  SD_STATE_DIS = 8,
  SD_STATE_INA = 15,  // inactive (not a CURRENT_STATE value)
} sd_state_t;

// Response kinds, also stored in the log.
typedef enum {
  SD_RESP_NONE = 0,
  SD_RESP_R1,
  SD_RESP_R1B,
  SD_RESP_R2,
  SD_RESP_R3,
  SD_RESP_R6,
  SD_RESP_R7,
} sd_resp_kind_t;

// Behaviour switches, set from the persistent config before core1 starts.
typedef struct {
  bool pcie_ack;      // set the PCIe availability bit in R7 when asked
  bool pcie_1v2;      // also set the PCIe 1.2 V bit when asked
  bool sd_mode;       // answer the SD identification after CMD8
  bool check_crc;     // drop commands with a bad CRC7, like a real card
  uint8_t acmd41_busy;  // ACMD41 answers reporting busy before ready
  uint16_t rca;
  uint32_t ocr;       // VDD window + CCS etc., busy bit excluded
  uint8_t cid[16];    // CRC7 byte filled in by sd_card_init()
  uint8_t csd[16];
  uint8_t scr[8];     // ACMD51
  uint8_t ssr[64];    // ACMD13, DAT_BUS_WIDTH filled in per command
  bool scr_4bit;      // SCR claims a 4-bit bus: accept ACMD6 to 4 bits
  uint8_t cmd_support;  // SCR CMD_SUPPORT: bit 0 CMD20, bit 1 CMD23
  uint16_t fg_support[6];  // CMD6 support bits, function group 1..6
  uint16_t max_current;    // CMD6 status, mA
} sd_card_cfg_t;

// A data block on DAT0 (1-bit bus): start bit, payload MSB first, CRC16, end
// bit. Laid out for the DAT state machine: [0] = clocks to wait before the
// start bit, minus one; [1] = frame bits, minus one; then the frame.
#define SD_DAT_MAX_BYTES 64u
#define SD_DAT_FRAME_WORDS ((SD_DAT_MAX_BYTES + 3u + 3u) / 4u)
#define SD_DAT_WORDS (2u + SD_DAT_FRAME_WORDS)
// Long enough for the R1 to be over before the block starts.
#define SD_DAT_DELAY_CLOCKS 80u

typedef struct {
  uint32_t words[SD_DAT_WORDS];
  uint8_t nwords;  // including the two header words
} sd_dat_t;

typedef struct {
  const sd_card_cfg_t *cfg;
  sd_state_t state;
  bool app_cmd;        // previous command was CMD55
  uint8_t acmd41_count;
  uint32_t pending_err;  // error bits reported once, in the next R1
  bool pcie_acked;     // CMD8 answered with the PCIe bit at least once
  uint8_t fg_current[6];   // CMD6 function currently selected per group
  uint8_t cmd6_status[64];  // payload of the last CMD6
  uint8_t ssr_now[64];      // payload of the last ACMD13
  uint8_t bus_width;        // 1 or 4, set by ACMD6
} sd_card_t;

// A decoded host command.
typedef struct {
  uint8_t index;   // 0-63
  uint32_t arg;
  uint8_t crc;     // as received
  bool crc_ok;
  bool from_host;  // transmission bit; false means this was a card response
  bool end_ok;     // end bit is 1
} sd_cmd_t;

// A response ready for the PIO: `words` hold the frame MSB first, as the
// state machine shifts them out; `nbits` is 0 for "no response".
typedef struct {
  uint32_t words[5];
  uint8_t nbits;
  uint8_t kind;  // sd_resp_kind_t
  uint32_t arg;  // the 32-bit payload of 48-bit responses, for the log
  // A data block to send on DAT0 after the response, or dat_len = 0. Framed
  // by sd_build_dat() only once the response is queued, so as not to eat
  // into the NCR budget.
  const uint8_t *dat;
  uint8_t dat_len;
  // The command wanted a block, but the bus is 4 bits wide: none was sent.
  bool dat_skipped;
} sd_resp_t;

// Flags summarising one handled command, for the log.
#define SD_LOGF_CRC_OK 0x01u
#define SD_LOGF_APP 0x02u        // index is an ACMD
#define SD_LOGF_RESPONDED 0x04u
#define SD_LOGF_PCIE_ACK 0x08u   // R7 carried the PCIe bit
#define SD_LOGF_BAD_FRAME 0x10u  // transmission/end bit wrong
#define SD_LOGF_ILLEGAL 0x20u    // not supported in this state
#define SD_LOGF_DATA 0x40u       // followed by a data block on DAT0

uint8_t sd_crc7(const uint8_t *data, unsigned len);
uint16_t sd_crc16(const uint8_t *data, unsigned len);

// Frames `len` (<= SD_DAT_MAX_BYTES) payload bytes for the DAT state machine.
void sd_build_dat(sd_dat_t *d, const uint8_t *data, unsigned len);

// Fills in the CRC bytes of cfg->cid / cfg->csd and resets the card.
// cfg->scr / ssr / fg_support / max_current must be set (see the builders).
void sd_card_init(sd_card_t *card, sd_card_cfg_t *cfg);
void sd_card_reset(sd_card_t *card);

// The two words the PIO receiver pushes for one 48-bit frame.
void sd_decode(uint32_t w0, uint32_t w1, sd_cmd_t *cmd);

// Runs one command through the state machine. Returns the SD_LOGF_* flags.
uint8_t sd_card_handle(sd_card_t *card, const sd_cmd_t *cmd, sd_resp_t *resp);

// Helpers for building the static register contents.
void sd_build_csd_v2(uint8_t csd[16], uint32_t c_size);
void sd_build_scr(uint8_t scr[8], uint32_t c_size, bool four_bit,
                  uint8_t cmd_support);
void sd_build_ssr(uint8_t ssr[64]);
void sd_build_cid(uint8_t cid[16], uint8_t mid, const char oid[2],
                  const char pnm[5], uint8_t prv, uint32_t psn,
                  unsigned year, unsigned month);

#endif  // SD_PROTO_H
