#include "sd_responder.h"

#include <stdio.h>

#include "board.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/structs/m33.h"
#include "pico/multicore.h"
#include "pico/unique_id.h"
#include "sd_cmd.pio.h"
#include "sd_proto.h"
#include "sdlog.h"

// CMD and DAT0 programs do not fit in one PIO's 32 instructions together.
#define SD_PIO pio0
#define SD_SM 0u
#define DAT_PIO pio1
#define DAT_SM 0u

static sd_card_cfg_t card_cfg;
static sd_card_t card;

// DAT0 blocks are fed to the PIO by DMA so that core1 is free for the CMD
// line meanwhile. Two buffers: one may still be in flight while the next is
// framed.
static sd_dat_t dat_buf[2];
static unsigned dat_next;
static uint dat_dma;
static uint dat_offset;

// ---------------------------------------------------------------------------
// Launch phase (see sd_cmd.pio): which CLK edge a transmitter launches its
// bits from, and whether CLK skips the input synchroniser. RAM copies of the
// WAITs in each program's transmit section, so that core1 can rewrite them.

typedef struct {
  PIO pio;
  uint offset;
  uint8_t n;
  uint8_t idx[16];   // instruction index within the program
  uint16_t ins[16];  // as assembled: the falling-edge form
} phase_ctl_t;

static phase_ctl_t cmd_phase, dat_phase;
static uint8_t dat_phase_now;
static bool dat_phase_sweep;
static uint32_t attempts;

static void phase_init(phase_ctl_t *p, PIO pio, uint offset,
                       const pio_program_t *prog, uint tx) {
  p->pio = pio;
  p->offset = offset;
  p->n = 0;
  for (uint i = tx; i < prog->length && p->n < count_of(p->idx); i++) {
    uint16_t ins = prog->instructions[i];
    if ((ins & 0xE000u) == 0x2000u) {  // WAIT
      p->idx[p->n] = (uint8_t)i;
      p->ins[p->n] = ins;
      p->n++;
    }
  }
}

// Only call while the state machine is outside the transmit section.
static void __not_in_flash_func(phase_set)(phase_ctl_t *p, unsigned phase) {
  bool rising = phase & SD_PHASE_RISING;
  for (unsigned i = 0; i < p->n; i++)
    p->pio->instr_mem[p->offset + p->idx[i]] = p->ins[i] ^ (rising ? 0x80u : 0u);
  if (phase & SD_PHASE_FAST)
    hw_set_bits(&p->pio->input_sync_bypass, 1u << PIN_SD_CLK);
  else
    hw_clear_bits(&p->pio->input_sync_bypass, 1u << PIN_SD_CLK);
}

// Pad drive setting (config cmd_drive / dat_drive).
static void set_pad(uint pin, uint8_t drive) {
  static const enum gpio_drive_strength ma[4] = {
      GPIO_DRIVE_STRENGTH_4MA, GPIO_DRIVE_STRENGTH_4MA, GPIO_DRIVE_STRENGTH_8MA,
      GPIO_DRIVE_STRENGTH_12MA};
  drive &= 3u;
  gpio_set_drive_strength(pin, ma[drive]);
  gpio_set_slew_rate(pin, drive ? GPIO_SLEW_RATE_FAST : GPIO_SLEW_RATE_SLOW);
}

// Sweep mode: each CMD0 (the host's next attempt) moves DAT0 on to the next
// phase, so one insertion tries all four.
static void __not_in_flash_func(sweep_next)(void) {
  attempts++;
  if (!dat_phase_sweep) return;
  bool idle = pio_sm_get_pc(DAT_PIO, DAT_SM) == dat_offset + sd_dat_offset_idle &&
              pio_sm_is_tx_fifo_empty(DAT_PIO, DAT_SM) &&
              !dma_channel_is_busy(dat_dma);
  if (!idle) return;  // a block still going out: keep the phase
  dat_phase_now = (uint8_t)((attempts - 1u) & 3u);
  phase_set(&dat_phase, dat_phase_now);
}

// ---------------------------------------------------------------------------

// core1: everything from here on runs from RAM, because core0 erases and
// programs flash whenever it saves the log. No interrupts, no SDK calls that
// are not inline.
static void __not_in_flash_func(core1_main)(void) {
  PIO pio = SD_PIO;
  sd_cmd_t cmd;
  sd_resp_t resp;

  // Cycle counter, to measure the SD clock from the gap between the
  // receiver's two words, which arrive 16 SD clocks apart.
  m33_hw->demcr |= M33_DEMCR_TRCENA_BITS;
  m33_hw->dwt_ctrl |= M33_DWT_CTRL_CYCCNTENA_BITS;

  for (;;) {
    uint32_t w0 = pio_sm_get_blocking(pio, SD_SM);
    uint32_t t0 = m33_hw->dwt_cyccnt;
    // CMD0 never gets a response and the host may follow it after only 8
    // clocks, which is tight on the boot oscillator. The first word already
    // holds the index, 16 clocks before the frame ends, so queue the "no
    // response" header now.
    bool early = ((w0 >> 24) & 0x3Fu) == 0;
    if (early) pio_sm_put_blocking(pio, SD_SM, 0);

    uint32_t w1 = pio_sm_get_blocking(pio, SD_SM);
    uint32_t clk16 = m33_hw->dwt_cyccnt - t0;
    sd_decode(w0, w1, &cmd);
    uint8_t flags = sd_card_handle(&card, &cmd, &resp);

    // Answer before logging: the deadline is 64 clocks.
    if (!early) {
      pio_sm_put_blocking(pio, SD_SM, resp.nbits ? resp.nbits - 1u : 0u);
      for (unsigned i = 0; i * 32u < resp.nbits; i++)
        pio_sm_put_blocking(pio, SD_SM, resp.words[i]);
    }

    // Then the data block, if any. The DAT program waits
    // SD_DAT_DELAY_CLOCKS before the start bit, so the R1 is out first.
    if (resp.dat_len) {
      sd_dat_t *d = &dat_buf[dat_next];
      dat_next ^= 1u;
      sd_build_dat(d, resp.dat, resp.dat_len);
      while (dma_channel_is_busy(dat_dma)) tight_loop_contents();
      dma_channel_transfer_from_buffer_now(dat_dma, d->words, d->nwords);
    }

    if (early && cmd.from_host) sweep_next();

    sdlog_add(cmd.arg, resp.arg, cmd.index, flags, resp.kind,
              (uint8_t)card.state, clk16, dat_phase_now,
              (uint8_t)((card.bus_width == 4 ? SDLOG_X_4BIT : 0u) |
                        (resp.dat_skipped ? SDLOG_X_DAT_SKIPPED : 0u)));
  }
}

void sd_responder_start(const kg_config_t *cfg) {
  pico_unique_board_id_t uid;
  pico_get_unique_board_id(&uid);
  uint32_t psn = (uint32_t)uid.id[4] << 24 | (uint32_t)uid.id[5] << 16 |
                 (uint32_t)uid.id[6] << 8 | uid.id[7];

  card_cfg.pcie_ack = cfg->pcie_ack;
  card_cfg.pcie_1v2 = cfg->pcie_1v2;
  card_cfg.sd_mode = cfg->sd_mode;
  card_cfg.check_crc = cfg->check_crc;
  card_cfg.acmd41_busy = cfg->acmd41_busy;
  card_cfg.rca = cfg->rca ? cfg->rca : (uint16_t)(psn ^ (psn >> 16));
  if (!card_cfg.rca) card_cfg.rca = 0x0001;
  card_cfg.ocr = SD_OCR_VDD_MASK | SD_OCR_CCS;
  sd_build_cid(card_cfg.cid, cfg->mid, "KG", "KGEXP", 0x10, psn, 2026, 11);
  sd_build_csd_v2(card_cfg.csd, cfg->c_size);
  card_cfg.scr_4bit = cfg->scr_4bit;
  card_cfg.cmd_support = cfg->scr_cmd;
  sd_build_scr(card_cfg.scr, cfg->c_size, cfg->scr_4bit, cfg->scr_cmd);
  sd_build_ssr(card_cfg.ssr);
  for (int g = 0; g < 6; g++) card_cfg.fg_support[g] = cfg->cmd6_fg[g];
  card_cfg.max_current = 100;
  sd_card_init(&card, &card_cfg);

  // DAT3 is an input only. A real card has a 50 k pull-up on it (hosts may
  // use it for card detect); the RP2350's is about the same. DAT0 belongs
  // to the DAT program, which only drives it during a data block.
  gpio_pull_up(PIN_SD_DAT0);
  gpio_init(PIN_SD_DAT3);
  if (cfg->dat3_pullup)
    gpio_pull_up(PIN_SD_DAT3);
  else
    gpio_disable_pulls(PIN_SD_DAT3);
  // Keep CMD and CLK defined while nothing drives them (e.g. while the
  // adapter has switched the lines over to the SSD).
  gpio_pull_up(PIN_SD_CMD);
  gpio_pull_up(PIN_SD_CLK);

  // Phases are set before the state machines start, so no transmit section
  // can be running yet.
  uint offset = pio_add_program(SD_PIO, &sd_cmd_program);
  phase_init(&cmd_phase, SD_PIO, offset, &sd_cmd_program, sd_cmd_offset_launch);
  phase_set(&cmd_phase, cfg->cmd_phase & 3u);
  pio_sm_claim(SD_PIO, SD_SM);
  sd_cmd_program_init(SD_PIO, SD_SM, offset, PIN_SD_CMD);

  dat_offset = pio_add_program(DAT_PIO, &sd_dat_program);
  phase_init(&dat_phase, DAT_PIO, dat_offset, &sd_dat_program, sd_dat_offset_launch);
  dat_phase_sweep = cfg->dat_phase == SD_PHASE_SWEEP;
  dat_phase_now = dat_phase_sweep ? 0 : (uint8_t)(cfg->dat_phase & 3u);
  phase_set(&dat_phase, dat_phase_now);
  pio_sm_claim(DAT_PIO, DAT_SM);
  sd_dat_program_init(DAT_PIO, DAT_SM, dat_offset, PIN_SD_DAT0, PIN_SD_CLK);

  dat_dma = (uint)dma_claim_unused_channel(true);
  dma_channel_config dc = dma_channel_get_default_config(dat_dma);
  channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
  channel_config_set_read_increment(&dc, true);
  channel_config_set_write_increment(&dc, false);
  channel_config_set_dreq(&dc, pio_get_dreq(DAT_PIO, DAT_SM, true));
  dma_channel_configure(dat_dma, &dc, &DAT_PIO->txf[DAT_SM], NULL, 0, false);

  set_pad(PIN_SD_CMD, cfg->cmd_drive);
  set_pad(PIN_SD_DAT0, cfg->dat_drive);

  multicore_launch_core1(core1_main);
}

// ---------------------------------------------------------------------------
// DAT0 loopback self-test. pio2 plays the host: it drives CLK and samples
// DAT0 back from the pin while the real DAT program (pio1, fed by the same
// DMA channel as in service) sends blocks. Checks the PIO/DMA/pad path on
// the chip itself, in every launch phase; it cannot say how the Switch 2's
// own sampling sees the line.

#define TEST_PIO pio2
#define TEST_SM 0u
#define TEST_WORDS 96u  // 3072 clocks captured: delay + 530-bit block fits

static int test_offset = -1;

static int frame_bit(const uint32_t *w, unsigned i) {
  return (int)((w[i / 32u] >> (31u - i % 32u)) & 1u);
}

// Puts the DAT program back at its idle PULL with DAT0 released, whatever
// state a failed run left it in.
static void dat_sm_reset(void) {
  pio_sm_set_enabled(DAT_PIO, DAT_SM, false);
  pio_sm_clear_fifos(DAT_PIO, DAT_SM);
  pio_sm_restart(DAT_PIO, DAT_SM);
  pio_sm_set_pindirs_with_mask(DAT_PIO, DAT_SM, 0, 1u << PIN_SD_DAT0);
  pio_sm_exec(DAT_PIO, DAT_SM, pio_encode_jmp(dat_offset + sd_dat_offset_idle));
  pio_sm_set_enabled(DAT_PIO, DAT_SM, true);
}

// One block at one clock rate. Returns true if the frame read back exactly;
// *start = clocks before the start bit, *bad = first wrong bit (or -1 when
// no start bit was seen).
static bool loopback_block(float clkdiv, const uint8_t *payload, unsigned len,
                           int *start, int *bad) {
  static uint32_t cap[TEST_WORDS];
  static sd_dat_t d;
  sd_build_dat(&d, payload, len);
  unsigned nbits = 8u * len + 18u;

  uint ch = (uint)dma_claim_unused_channel(true);
  dma_channel_config c = dma_channel_get_default_config(ch);
  channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
  channel_config_set_read_increment(&c, false);
  channel_config_set_write_increment(&c, true);
  channel_config_set_dreq(&c, pio_get_dreq(TEST_PIO, TEST_SM, false));

  sd_selftest_host_init(TEST_PIO, TEST_SM, (uint)test_offset, PIN_SD_CLK,
                        PIN_SD_DAT0, clkdiv);
  dma_channel_configure(ch, &c, cap, &TEST_PIO->rxf[TEST_SM], TEST_WORDS, true);
  pio_sm_set_enabled(TEST_PIO, TEST_SM, true);
  dma_channel_transfer_from_buffer_now(dat_dma, d.words, d.nwords);
  dma_channel_wait_for_finish_blocking(ch);
  pio_sm_set_enabled(TEST_PIO, TEST_SM, false);
  dma_channel_unclaim(ch);
  dma_channel_abort(dat_dma);
  dat_sm_reset();

  *start = -1;
  *bad = -1;
  for (unsigned i = 0; i + nbits <= TEST_WORDS * 32u; i++) {
    if (frame_bit(cap, i) == 0) {
      *start = (int)i;
      break;
    }
  }
  if (*start < 0) return false;
  for (unsigned i = 0; i < nbits; i++) {
    if (frame_bit(cap, (unsigned)*start + i) != frame_bit(&d.words[2], i)) {
      *bad = (int)i;
      return false;
    }
  }
  return true;
}

void sd_responder_selftest(void) {
  // Never drive CLK against a host: it must be idle (pulled up, no edges)
  // and no command may arrive meanwhile.
  uint32_t n0 = sdlog_count;
  absolute_time_t until = make_timeout_time_ms(20);
  while (!time_reached(until)) {
    if (!gpio_get(PIN_SD_CLK)) {
      printf("selftest: CLK is low or toggling - take the board out of the "
             "adapter first\n");
      return;
    }
  }
  if (sdlog_count != n0) {
    printf("selftest: SD commands are arriving - take the board out first\n");
    return;
  }
  if (test_offset < 0) test_offset = pio_add_program(TEST_PIO, &sd_selftest_host_program);
  pio_sm_claim(TEST_PIO, TEST_SM);

  static uint8_t pattern[64], zeros[64];
  for (unsigned i = 0; i < 64; i++) pattern[i] = (uint8_t)(i * 37u + 5u);
  for (unsigned i = 0; i < 64; i++) zeros[i] = 0;
  zeros[17] = 0x01;  // like a CMD6 status: almost all zero
  const struct {
    const char *name;
    const uint8_t *data;
    unsigned len;
  } blocks[] = {
      {"SCR(8B)", card_cfg.scr, 8},
      {"SSR(64B)", card_cfg.ssr, 64},
      {"pattern(64B)", pattern, 64},
      {"zeros(64B)", zeros, 64},
  };
  const struct {
    const char *name;
    float div;
  } rates[] = {{"25MHz", 1.0f}, {"12.5MHz", 2.0f}, {"400kHz", 62.5f}};

  printf("selftest: DAT0 loopback, CLK from pio2, DAT0 read back at the pin\n");
  bool all_ok = true;
  for (unsigned r = 0; r < count_of(rates); r++) {
    for (unsigned ph = 0; ph < 4; ph++) {
      phase_set(&dat_phase, ph);
      printf("  %-7s dph=%u:", rates[r].name, ph);
      for (unsigned b = 0; b < count_of(blocks); b++) {
        int start, bad;
        bool ok = loopback_block(rates[r].div, blocks[b].data, blocks[b].len,
                                 &start, &bad);
        all_ok &= ok;
        if (ok)
          printf("  %s ok", blocks[b].name);
        else if (start < 0)
          printf("  %s NO START BIT", blocks[b].name);
        else
          printf("  %s BAD at bit %d", blocks[b].name, bad);
      }
      printf("\n");
    }
  }
  phase_set(&dat_phase, dat_phase_now);

  // Give CLK back to the CMD program's PIO, as an input again.
  pio_sm_set_pindirs_with_mask(TEST_PIO, TEST_SM, 0, 1u << PIN_SD_CLK);
  pio_gpio_init(SD_PIO, PIN_SD_CLK);
  pio_sm_unclaim(TEST_PIO, TEST_SM);
  printf("selftest: %s\n", all_ok ? "all blocks read back intact"
                                  : "FAILED (see above)");
}
