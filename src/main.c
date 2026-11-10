// KagigataDisk as an SD Express handshake stand-in for a microSD Express to
// M.2 adapter (Switch 2). The host's CMD0/CMD8 reach this board while VDD2 is
// off; CMD8 is answered with the PCIe bit set, the host turns VDD2 on, and the
// adapter hands the slot over to the SSD.
//
// core1: answers the CMD line (sd_responder.c), from RAM.
// core0: everything else - clocks, USB console, LEDs, saving the log.
//
// Boot order matters. The SDK normally sets up the crystal and PLLs before
// main(), which takes several milliseconds; a host may already be sending
// CMD0/CMD8 by then. So clock setup is skipped in the runtime init (see
// CMakeLists.txt) and done here, after the responder is running on the boot
// ring oscillator.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "config.h"
#include "hardware/gpio.h"
#include "hardware/ticks.h"
#include "hardware/watchdog.h"
#include "pico/bootrom.h"
#include "pico/runtime_init.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "sd_responder.h"
#include "sdlog.h"
#include "tusb.h"

#ifndef KG_FW_VERSION
#define KG_FW_VERSION "dev"
#endif

// ---------------------------------------------------------------------------
// LEDs

#define LED_ACTIVITY_HOLD_MS 30
#define LED_USB_BLINK_MS 250

static void led_init(void) {
  const uint pins[] = {PIN_LED1, PIN_LED2, PIN_LED3};
  for (unsigned i = 0; i < count_of(pins); i++) {
    gpio_init(pins[i]);
    gpio_set_dir(pins[i], GPIO_OUT);
    gpio_put(pins[i], 0);
  }
}

static void led_update(void) {
  static uint32_t last_count;
  static absolute_time_t activity_off, blink_at;
  static bool blink;
  absolute_time_t now = get_absolute_time();

  // LED1: on = running; blinking = USB host attached.
  if (tud_mounted()) {
    if (absolute_time_diff_us(blink_at, now) >= 0) {
      blink = !blink;
      blink_at = delayed_by_ms(now, LED_USB_BLINK_MS);
    }
    gpio_put(PIN_LED1, blink);
  } else {
    gpio_put(PIN_LED1, 1);
  }

  // LED2: command activity.
  uint32_t n = sdlog_count;
  if (n != last_count) {
    last_count = n;
    activity_off = delayed_by_ms(now, LED_ACTIVITY_HOLD_MS);
  }
  gpio_put(PIN_LED2, absolute_time_diff_us(now, activity_off) > 0);

  // LED3: CMD8 answered with the PCIe bit this boot.
  gpio_put(PIN_LED3, sdlog_pcie_acked());
}

// ---------------------------------------------------------------------------
// USB console

static void print_help(void) {
  printf(
      "kagigatainject " KG_FW_VERSION " - SD Express handshake for KagigataDisk\n"
      "  log            commands seen since this boot\n"
      "  hist           saved boots (newest first); 'hist N' prints one\n"
      "  clear          erase saved boots\n"
      "  cfg            show settings\n"
      "  set NAME VAL   change a setting (saved; applies after reboot)\n"
      "  defaults       restore default settings (saved)\n"
      "  reboot         restart the board\n"
      "  bootsel        restart into the USB bootloader\n"
      "  selftest run   DAT0 loopback test; the board drives CLK, so only\n"
      "                 with the board out of any slot or adapter\n"
      "New commands are printed as they arrive while a terminal is open.\n");
}

static void run_command(char *line) {
  char *argv[4] = {0};
  int argc = 0;
  for (char *tok = strtok(line, " \t"); tok && argc < 4;
       tok = strtok(NULL, " \t"))
    argv[argc++] = tok;
  if (argc == 0) return;

  if (!strcmp(argv[0], "help") || !strcmp(argv[0], "?")) {
    print_help();
  } else if (!strcmp(argv[0], "log")) {
    sdlog_print_ram();
  } else if (!strcmp(argv[0], "hist")) {
    sdlog_print_history(argc > 1 ? atoi(argv[1]) : -1);
  } else if (!strcmp(argv[0], "clear")) {
    sdlog_erase_history();
    printf("saved boots erased\n");
  } else if (!strcmp(argv[0], "cfg")) {
    config_print();
  } else if (!strcmp(argv[0], "set") && argc == 3) {
    if (config_set(argv[1], argv[2])) {
      config_save();
      printf("saved; 'reboot' to apply\n");
    }
  } else if (!strcmp(argv[0], "defaults")) {
    config_defaults(&kg_config);
    config_save();
    printf("defaults saved; 'reboot' to apply\n");
  } else if (!strcmp(argv[0], "selftest")) {
    if (argc == 2 && !strcmp(argv[1], "run"))
      sd_responder_selftest();
    else
      printf("selftest drives the CLK pin: take the board out of any slot or "
             "adapter, then type 'selftest run'\n");
  } else if (!strcmp(argv[0], "reboot")) {
    printf("rebooting\n");
    sleep_ms(50);
    watchdog_reboot(0, 0, 0);
  } else if (!strcmp(argv[0], "bootsel")) {
    printf("entering BOOTSEL\n");
    sleep_ms(50);
    rom_reset_usb_boot_extra(-1, 0, false);
  } else {
    printf("unknown command; 'help' lists them\n");
  }
}

static void console_poll(void) {
  static char line[64];
  static unsigned len;
  static bool was_connected;

  bool connected = stdio_usb_connected();
  if (connected && !was_connected) {
    printf("\n");
    print_help();
    printf("> ");
  }
  was_connected = connected;

  int c;
  while ((c = getchar_timeout_us(0)) != PICO_ERROR_TIMEOUT) {
    if (c == '\r' || c == '\n') {
      printf("\n");
      line[len] = 0;
      len = 0;
      run_command(line);
      printf("> ");
    } else if ((c == 8 || c == 127) && len) {
      len--;
      printf("\b \b");
    } else if (c >= ' ' && c < 127 && len < sizeof line - 1) {
      line[len++] = (char)c;
      putchar(c);
    }
  }
}

// ---------------------------------------------------------------------------

int main(void) {
  // Still on the ring oscillator here. Start the timer from it (roughly 1 us
  // per tick; exact once runtime_init_clocks() switches to the crystal) so
  // that the earliest log entries get usable timestamps.
  tick_start(TICK_TIMER0, 11);
  config_load();
  sd_responder_start(&kg_config);

  led_init();
  gpio_put(PIN_LED1, 1);  // scope this against VDD to see the boot latency

  runtime_init_clocks();
  runtime_init_post_clock_resets();
  sdlog_sysclk_ready = true;  // from here on the log can convert SD clocks

  stdio_init_all();
  sdlog_flash_init();

  for (;;) {
    sdlog_print_new();
    console_poll();
    sdlog_service();
    led_update();
  }
}
