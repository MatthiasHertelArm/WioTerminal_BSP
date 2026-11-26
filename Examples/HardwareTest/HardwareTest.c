/* Copyright (c) 2026 Arm Limited (or its affiliates).
 * SPDX-License-Identifier: Apache-2.0
 */
#include "wio_sdk.h"
#include "cmsis_vio.h"
#include "wio_drivers.h"
#include "wio_adc.h"
#include "wio_i2c.h"
#include "wio_lcd.h"
#include "wio_flash.h"
#include "wio_fs.h"
#include "wio_sd.h"
#include "wio_network.h"
#include "lis3dh.h"
#include "HardwareTest.h"
#include <stddef.h>

#define ALL_BUTTONS (vioBUTTON0 | vioBUTTON1 | vioBUTTON2 | vioJOYall)
#define ADC_SAMPLES 32U
#define ACCEL_SAMPLES 8U

volatile hardware_test_results_t g_hw_test;
static lis3dh_t accelerometer;

static void i2c_event(uint32_t event) {
  g_hw_test.i2c_events |= event;
  if ((event & ARM_I2C_EVENT_TRANSFER_DONE) != 0U) {
    ++g_hw_test.i2c_completions;
  }
}

static void spi_event(uint32_t event) {
  g_hw_test.spi_events |= event;
  if ((event & ARM_SPI_EVENT_TRANSFER_COMPLETE) != 0U) {
    ++g_hw_test.spi_completions;
  }
}

static void summarize(void) {
  const hardware_test_checks_t c = g_hw_test.checks;
  const hw_result_t results[] = {
    c.clock, c.bootloader_protection, c.driver_api, c.sercom0_ownership,
    c.led_gpio, c.led_visual,
    c.buttons, c.accelerometer_id, c.accelerometer_samples, c.light_adc,
    c.microphone_adc, c.lcd_spi, c.lcd_visual, c.sdk_arguments, c.flash_id,
    c.flash_read, c.sd_card, c.filesystem, c.filesystem_fixture,
    c.external_interfaces, c.radio, c.network_link, c.usb,
    c.buzzer, c.infrared
  };
  g_hw_test.passed = g_hw_test.failed = g_hw_test.manual = g_hw_test.skipped = 0U;
  g_hw_test.not_run = 0U;
  for (uint32_t i = 0U; i < sizeof(results) / sizeof(results[0]); ++i) {
    if (results[i] == HW_PASS) { ++g_hw_test.passed; }
    if (results[i] == HW_FAIL) { ++g_hw_test.failed; }
    if (results[i] == HW_MANUAL) { ++g_hw_test.manual; }
    if (results[i] == HW_SKIPPED) { ++g_hw_test.skipped; }
    if (results[i] == HW_NOT_RUN) { ++g_hw_test.not_run; }
  }
}

static void record_sdk_arguments(uint32_t mask, bool passed) {
  g_hw_test.sdk_arguments_run |= mask;
  if (!passed) {
    g_hw_test.sdk_arguments_failed |= mask;
  }
  /* A missing initialization must never masquerade as argument rejection.
   * Retain a genuine failure even if another prerequisite was unavailable.
   */
  if (g_hw_test.sdk_arguments_failed != 0U) {
    g_hw_test.checks.sdk_arguments = HW_FAIL;
  } else if (g_hw_test.sdk_arguments_run == HW_ARGUMENT_ALL) {
    g_hw_test.checks.sdk_arguments = HW_PASS;
  } else {
    g_hw_test.checks.sdk_arguments = HW_NOT_RUN;
  }
}

static void test_driver_api(void) {
  /* Identity queries and invalid arguments do not drive connectors. */
  uint8_t byte = 0U;
  bool ok = Driver_I2C3.GetVersion().api == ARM_I2C_API_VERSION &&
            Driver_I2C4.GetVersion().api == ARM_I2C_API_VERSION &&
            Driver_SPI0.GetVersion().api == ARM_SPI_API_VERSION &&
            Driver_SPI5.GetVersion().api == ARM_SPI_API_VERSION &&
            Driver_SPI6.GetVersion().api == ARM_SPI_API_VERSION &&
            Driver_SPI7.GetVersion().api == ARM_SPI_API_VERSION &&
            Driver_USART0.GetVersion().api == ARM_USART_API_VERSION &&
            Driver_USART1.GetVersion().api == ARM_USART_API_VERSION &&
            Driver_USART2.GetVersion().api == ARM_USART_API_VERSION &&
            Driver_Flash0.GetVersion().api == ARM_FLASH_API_VERSION;
  ok = ok && Driver_SPI7.Send(NULL, 1U) == ARM_DRIVER_ERROR_PARAMETER &&
       Driver_I2C4.MasterTransmit(0x80U, &byte, 1U, false) == ARM_DRIVER_ERROR_PARAMETER &&
       Driver_USART2.Send(NULL, 1U) == ARM_DRIVER_ERROR_PARAMETER &&
       Driver_Flash0.ReadData(WIO_FLASH_SIZE_BYTES, &byte, 1U) == ARM_DRIVER_ERROR_PARAMETER;
  /* Register both callbacks regardless of unrelated API-test failures. */
  const bool i2c_initialized = Driver_I2C4.Initialize(i2c_event) == ARM_DRIVER_OK;
  const bool spi_initialized = Driver_SPI7.Initialize(spi_event) == ARM_DRIVER_OK;
  g_hw_test.checks.driver_api =
      ok && i2c_initialized && spi_initialized ? HW_PASS : HW_FAIL;
}

static void test_sercom0_ownership(void) {
  /* Power-only checks do not enable TX/RX or mux/drive the radio pins. */
  bool ok = Driver_USART0.Initialize(NULL) == ARM_DRIVER_OK;
  ok = (Driver_SPI0.Initialize(NULL) == ARM_DRIVER_OK) && ok;
  if (ok) {
    ok = Driver_USART0.PowerControl(ARM_POWER_FULL) == ARM_DRIVER_OK;
    if (ok) {
      ok = Driver_SPI0.PowerControl(ARM_POWER_FULL) == ARM_DRIVER_ERROR_BUSY;
      ok = (Driver_SPI0.PowerControl(ARM_POWER_OFF) == ARM_DRIVER_OK) && ok;
      /* Powering off the inactive SPI instance must not release the UART. */
      ok = (Driver_SPI0.PowerControl(ARM_POWER_FULL) == ARM_DRIVER_ERROR_BUSY) && ok;
    }
    ok = (Driver_USART0.PowerControl(ARM_POWER_OFF) == ARM_DRIVER_OK) && ok;
    if (ok) {
      ok = Driver_SPI0.PowerControl(ARM_POWER_FULL) == ARM_DRIVER_OK;
      if (ok) {
        ok = Driver_USART0.PowerControl(ARM_POWER_FULL) == ARM_DRIVER_ERROR_BUSY;
        ok = (Driver_USART0.PowerControl(ARM_POWER_OFF) == ARM_DRIVER_OK) && ok;
        ok = (Driver_USART0.PowerControl(ARM_POWER_FULL) == ARM_DRIVER_ERROR_BUSY) && ok;
      }
    }
  }
  ok = (Driver_SPI0.Uninitialize() == ARM_DRIVER_OK) && ok;
  ok = (Driver_USART0.Uninitialize() == ARM_DRIVER_OK) && ok;
  g_hw_test.checks.sercom0_ownership = ok ? HW_PASS : HW_FAIL;
}

static void test_gpio(void) {
  vioInit();
  const uint32_t led_mask = 1UL << WIO_PIN_NUMBER(WIO_PIN_LED);
  vioSetSignal(vioLED0, vioLEDon);
  bool high = (wio_port(WIO_PIN_LED)->PORT_OUT & led_mask) != 0U;
  vioSetSignal(vioLED0, vioLEDoff);
  bool low = (wio_port(WIO_PIN_LED)->PORT_OUT & led_mask) == 0U;
  g_hw_test.checks.led_gpio = high && low ? HW_PASS : HW_FAIL;
  g_hw_test.checks.led_visual = HW_MANUAL;
  g_hw_test.checks.buttons = HW_MANUAL;
  /* Record the socket contact only; a logic level is not a card-read test. */
  wio_gpio_input(WIO_PIN_SD_DET, 1);
  g_hw_test.sd_detect_raw = wio_gpio_read(WIO_PIN_SD_DET);
  g_hw_test.checks.sd_card = HW_SKIPPED;
  g_hw_test.checks.filesystem = HW_SKIPPED;
  g_hw_test.checks.filesystem_fixture = HW_SKIPPED;
  g_hw_test.checks.external_interfaces = HW_SKIPPED;
  g_hw_test.checks.radio = HW_SKIPPED;
  g_hw_test.checks.network_link = HW_SKIPPED;
  g_hw_test.checks.usb = HW_SKIPPED;
  g_hw_test.checks.buzzer = HW_MANUAL;
  g_hw_test.checks.infrared = HW_SKIPPED;
}

static void test_accelerometer(void) {
  const bool bus_ready = wio_i2c_init(WIO_I2C_INTERNAL);
  if (bus_ready) {
    uint8_t raw = 0U;
    record_sdk_arguments(HW_ARGUMENT_I2C,
        !wio_i2c_read(WIO_I2C_INTERNAL, 0x80U, &raw, 1U));
  }
  bool ready = bus_ready && lis3dh_init(&accelerometer);
  g_hw_test.accel_address = accelerometer.address;
  g_hw_test.accel_who_am_i = accelerometer.who_am_i;
  g_hw_test.checks.accelerometer_id =
      accelerometer.who_am_i == LIS3DH_WHO_AM_I_VALUE ? HW_PASS : HW_FAIL;
  if (ready) {
    for (uint32_t i = 0U; i < ACCEL_SAMPLES; ++i) {
      lis3dh_sample_t sample;
      if (!lis3dh_read(&accelerometer, &sample)) { break; }
      ++g_hw_test.accel_samples;
      g_hw_test.accel_x_mg = sample.x_mg;
      g_hw_test.accel_y_mg = sample.y_mg;
      g_hw_test.accel_z_mg = sample.z_mg;
      g_hw_test.accel_norm_squared =
          (uint32_t)((int32_t)sample.x_mg * sample.x_mg +
                     (int32_t)sample.y_mg * sample.y_mg +
                     (int32_t)sample.z_mg * sample.z_mg);
    }
  }
  g_hw_test.checks.accelerometer_samples =
      ready && g_hw_test.accel_samples == ACCEL_SAMPLES ? HW_PASS : HW_FAIL;
}

static void test_adc(void) {
  uint32_t light_sum = 0U, mic_sum = 0U;
  g_hw_test.light_min = g_hw_test.mic_min = 4095U;
  if (wio_adc_init()) {
    uint16_t raw = 0U;
    record_sdk_arguments(HW_ARGUMENT_ADC, !wio_adc_read(0U, &raw));
    for (uint32_t i = 0U; i < ADC_SAMPLES; ++i) {
      uint16_t light, mic;
      if (wio_adc_read_light(&light)) {
        if (light < g_hw_test.light_min) { g_hw_test.light_min = light; }
        if (light > g_hw_test.light_max) { g_hw_test.light_max = light; }
        light_sum += light;
        ++g_hw_test.light_samples;
      }
      if (wio_adc_read_microphone(&mic)) {
        if (mic < g_hw_test.mic_min) { g_hw_test.mic_min = mic; }
        if (mic > g_hw_test.mic_max) { g_hw_test.mic_max = mic; }
        mic_sum += mic;
        ++g_hw_test.mic_samples;
      }
    }
  }
  if (g_hw_test.light_samples != 0U) {
    g_hw_test.light_mean = light_sum / g_hw_test.light_samples;
  }
  if (g_hw_test.mic_samples != 0U) {
    g_hw_test.mic_mean = mic_sum / g_hw_test.mic_samples;
  }
  g_hw_test.checks.light_adc = g_hw_test.light_samples == ADC_SAMPLES ? HW_PASS : HW_FAIL;
  g_hw_test.checks.microphone_adc = g_hw_test.mic_samples == ADC_SAMPLES ? HW_PASS : HW_FAIL;
}

static void test_lcd(void) {
  bool ok = wio_lcd_init();
  if (ok) {
    const bool invalid = !wio_lcd_fill_rect(319U, 0U, 2U, 1U, 0U) &&
                         !wio_lcd_fill_rect(0U, 0U, 0U, 1U, 0U) &&
                         !wio_lcd_write_rect(0U, 0U, 1U, 1U, NULL) &&
                         !wio_lcd_read_id(NULL);
    record_sdk_arguments(HW_ARGUMENT_LCD, invalid);
  }
  const uint16_t colors[4] = {0xF800U, 0x07E0U, 0x001FU, 0xFFFFU};
  for (uint32_t i = 0U; ok && i < 4U; ++i) {
    ok = wio_lcd_fill_rect((uint16_t)(80U * i), 0U, 80U, 240U, colors[i]);
  }
  uint32_t id = 0U;
  if (ok) {
    /* The board's LCD readback may be zero: never use it as a pass criterion. */
    ok = wio_lcd_read_id(&id);
    g_hw_test.lcd_id = id;
  }
  g_hw_test.lcd_transfer_count = Driver_SPI7.GetDataCount();
  g_hw_test.checks.lcd_spi = ok && g_hw_test.spi_completions != 0U ? HW_PASS : HW_FAIL;
  g_hw_test.checks.lcd_visual = HW_MANUAL;
}

static void test_flash(void) {
  uint8_t id[3], a[16], b[16];
  bool ok = Driver_Flash0.Initialize(NULL) == ARM_DRIVER_OK &&
            Driver_Flash0.PowerControl(ARM_POWER_FULL) == ARM_DRIVER_OK &&
            wio_flash_read_id(id);
  if (ok) {
    g_hw_test.flash_jedec_id = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
  }
  ok = ok && (g_hw_test.flash_jedec_id == WIO_FLASH_JEDEC_IQ ||
              g_hw_test.flash_jedec_id == WIO_FLASH_JEDEC_IM);
  g_hw_test.checks.flash_id = ok ? HW_PASS : HW_FAIL;
  ok = ok && Driver_Flash0.ReadData(0U, a, sizeof(a)) == (int32_t)sizeof(a) &&
             Driver_Flash0.ReadData(0U, b, sizeof(b)) == (int32_t)sizeof(b);
  uint32_t hash = 2166136261U;
  if (ok) {
    for (uint32_t i = 0U; i < sizeof(a); ++i) {
      ok = ok && a[i] == b[i];
      hash = (hash ^ a[i]) * 16777619U;
    }
    g_hw_test.flash_sample_hash = hash;
  }
  g_hw_test.checks.flash_read = ok ? HW_PASS : HW_FAIL;
  /* NEVER call program/erase, even to test their unsupported return codes. */
  (void)Driver_Flash0.PowerControl(ARM_POWER_OFF);
}

static uint32_t hash_bytes(const uint8_t *bytes, size_t count) {
  uint32_t hash = 2166136261U;
  for (size_t i = 0U; i < count; ++i) {
    hash = (hash ^ bytes[i]) * 16777619U;
  }
  return hash;
}

static void test_storage(void) {
  uint8_t sector[WIO_SD_SECTOR_SIZE];
  wio_sd_info_t info;
  wio_sd_result_t sd = wio_sd_init();
  if (sd != WIO_SD_OK) {
    g_hw_test.sd_result = (uint32_t)sd;
    /* A missing/unresponsive card is not an automated hardware PASS. */
    g_hw_test.checks.sd_card =
        sd == WIO_SD_NOT_READY || sd == WIO_SD_TIMEOUT ? HW_SKIPPED : HW_FAIL;
    (void)wio_sd_deinit();
    return;
  }
  sd = wio_sd_get_info(&info);
  if (sd == WIO_SD_OK && (info.writable || info.sector_size != WIO_SD_SECTOR_SIZE)) {
    sd = WIO_SD_IO;
  }
  if (sd == WIO_SD_OK) {
    g_hw_test.sd_sector_count = info.sector_count;
    g_hw_test.sd_sector_size = info.sector_size;
    g_hw_test.sd_block_addressed = info.block_addressed ? 1U : 0U;
    sd = wio_sd_read_blocks(0U, sector, 1U);
    if (sd == WIO_SD_OK) {
      g_hw_test.sd_sector0_hash = hash_bytes(sector, sizeof(sector));
    }
  }
  g_hw_test.sd_result = (uint32_t)sd;
  g_hw_test.checks.sd_card = sd == WIO_SD_OK ? HW_PASS : HW_FAIL;
  wio_sd_result_t deinit_result = wio_sd_deinit();
  if (deinit_result != WIO_SD_OK) {
    g_hw_test.sd_result = (uint32_t)deinit_result;
    g_hw_test.checks.sd_card = HW_FAIL;
  }
  if (deinit_result != WIO_SD_OK || sd != WIO_SD_OK) { return; }

  FRESULT fr = wio_fs_mount(false); /* NEVER opt into writes in this suite. */
  g_hw_test.fs_result = (uint32_t)fr;
  if (fr != FR_OK) {
    g_hw_test.checks.filesystem = fr == FR_NO_FILESYSTEM ? HW_SKIPPED : HW_FAIL;
    return;
  }
  if (wio_fs_is_writable()) {
    g_hw_test.checks.filesystem = HW_FAIL;
    g_hw_test.fs_unmount_result = (uint32_t)wio_fs_unmount();
    return;
  }
  DIR directory;
  FILINFO entry;
  fr = f_opendir(&directory, "0:/");
  if (fr == FR_OK) {
    while (g_hw_test.fs_entries < 64U) {
      fr = f_readdir(&directory, &entry);
      if (fr != FR_OK || entry.fname[0] == '\0') { break; }
      ++g_hw_test.fs_entries; /* Do not retain user filenames or file contents. */
    }
    if (g_hw_test.fs_entries == 64U) { g_hw_test.fs_scan_capped = 1U; }
    FRESULT close_result = f_closedir(&directory);
    if (fr == FR_OK) { fr = close_result; }
  }
  g_hw_test.fs_result = (uint32_t)fr;
  g_hw_test.checks.filesystem = fr == FR_OK ? HW_PASS : HW_FAIL;

  /* Only this deliberately named, user-provided fixture is opened. Missing is
   * SKIPPED; create it on a host if file-read coverage is wanted. Never create
   * or alter it here. Read at most one sector, retaining only count/hash. */
  if (fr == FR_OK) {
    FIL file;
    fr = f_open(&file, "0:/WIOTEST.TXT", FA_READ);
    if (fr == FR_OK) {
      UINT count = 0U;
      fr = f_read(&file, sector, sizeof(sector), &count);
      if (fr == FR_OK) {
        g_hw_test.fs_fixture_bytes = count;
        g_hw_test.fs_fixture_hash = hash_bytes(sector, count);
      }
      FRESULT close_result = f_close(&file);
      if (fr == FR_OK) { fr = close_result; }
      g_hw_test.checks.filesystem_fixture = fr != FR_OK ? HW_FAIL :
          (count != 0U ? HW_PASS : HW_SKIPPED);
    } else if (fr != FR_NO_FILE && fr != FR_NO_PATH) {
      g_hw_test.checks.filesystem_fixture = HW_FAIL;
    }
    g_hw_test.fs_fixture_result = (uint32_t)fr;
  }
  fr = wio_fs_unmount();
  g_hw_test.fs_unmount_result = (uint32_t)fr;
  if (fr != FR_OK) { g_hw_test.checks.filesystem = HW_FAIL; }
}

static void test_network(void) {
  wio_network_info_t info = {0};
  wio_network_result_t result = wio_network_init(&info, NULL, NULL, 2000U);
  g_hw_test.network_result = (int32_t)result;
  g_hw_test.network_baud = info.uart_baud;
  g_hw_test.network_echo_ok = info.echo_ok ? 1U : 0U;
  for (size_t i = 0U; i < sizeof(info.firmware); ++i) {
    g_hw_test.network_firmware[i] = info.firmware[i];
  }
  wio_network_diagnostics_t diagnostics = wio_network_get_diagnostics();
  g_hw_test.network_state = (uint32_t)diagnostics.state;
  g_hw_test.network_remote_result = diagnostics.remote_result;
  g_hw_test.network_remote_errno = diagnostics.remote_errno;
  g_hw_test.network_rpc_count = diagnostics.rpc_count;
  g_hw_test.network_event_count = diagnostics.event_count;
  g_hw_test.checks.radio = result == WIO_NETWORK_OK && info.echo_ok ? HW_PASS :
      (result == WIO_NETWORK_FIRMWARE ? HW_SKIPPED : HW_FAIL);
  /* No reset, radio enable, scan, Wi-Fi join, credentials, DNS or sockets. */
  (void)wio_network_deinit();
}

/* A stable debugger breakpoint; the test does not stop itself. */
__attribute__((noinline)) void hardware_test_checkpoint(void) {
  __NOP();
}

int main(void) {
  g_hw_test.magic = 0x57494F54U;
  g_hw_test.version = HARDWARE_TEST_REPORT_VERSION;
  g_hw_test.fs_result = HARDWARE_TEST_RESULT_UNSET;
  g_hw_test.fs_fixture_result = HARDWARE_TEST_RESULT_UNSET;
  g_hw_test.fs_unmount_result = HARDWARE_TEST_RESULT_UNSET;
  g_hw_test.phase = 1U;
  if (!wio_sdk_init()) {
    g_hw_test.checks.clock = HW_FAIL;
    g_hw_test.phase = 0xE001U;
    summarize();
    hardware_test_checkpoint();
    for (;;) { __WFI(); }
  }
  g_hw_test.core_clock_hz = SystemCoreClock;
  uint32_t tick_start = wio_millis();
  wio_delay_ms(10U);
  uint32_t elapsed = wio_millis() - tick_start;
  g_hw_test.checks.clock = elapsed >= 9U && elapsed <= 20U ? HW_PASS : HW_FAIL;
  g_hw_test.bootprot = (NVMCTRL_REGS->NVMCTRL_STATUS >> 8) & 0xFU;
  g_hw_test.checks.bootloader_protection = g_hw_test.bootprot == 13U ? HW_PASS : HW_FAIL;
  g_hw_test.phase = 2U;
  test_driver_api();
  test_sercom0_ownership();
  g_hw_test.phase = 3U;
  test_gpio();
  g_hw_test.phase = 4U;
  test_accelerometer();
  g_hw_test.phase = 5U;
  test_adc();
  g_hw_test.phase = 6U;
  test_lcd();
  g_hw_test.phase = 7U;
  test_flash();
  g_hw_test.phase = 8U;
  test_storage();
  g_hw_test.phase = 9U;
  test_network();
  g_hw_test.phase = 10U;
  g_hw_test.elapsed_ms = wio_millis();
  g_hw_test.completed = 1U;
  summarize();
  hardware_test_checkpoint();
  uint32_t last_buttons = 0U, stable_buttons = 0U, repeats = 0U;
  uint32_t led_tick = wio_millis();
  bool led_on = false;
  for (;;) {
    ++g_hw_test.heartbeat;
    uint32_t buttons = vioGetSignal(ALL_BUTTONS);
    if (buttons == last_buttons) {
      if (repeats < 3U) { ++repeats; }
    } else {
      repeats = 0U;
      last_buttons = buttons;
    }
    if (repeats == 3U) {
      const uint32_t released = stable_buttons & ~buttons;
      g_hw_test.buttons_released |= ~buttons & ALL_BUTTONS;
      const uint32_t pressed = buttons & ~stable_buttons & g_hw_test.buttons_released;
      g_hw_test.buttons_pressed |= pressed;
      g_hw_test.buttons_completed |= released & g_hw_test.buttons_pressed;
      stable_buttons = buttons;
      g_hw_test.buttons = buttons;
      g_hw_test.buttons_seen |= buttons;
      if ((g_hw_test.buttons_completed & ALL_BUTTONS) == ALL_BUTTONS &&
          g_hw_test.checks.buttons != HW_PASS) {
        g_hw_test.checks.buttons = HW_PASS;
        summarize();
      }
      /* A startup-held/stuck-low button is not a request. The first debounced
       * release arms BUTTON0; only a subsequent press requests the tone.
       */
      if ((pressed & vioBUTTON0) != 0U && wio_buzzer_tone(2000U, 80U)) {
        ++g_hw_test.buzzer_requests;
      }
    }
    if ((uint32_t)(wio_millis() - led_tick) >= 500U) {
      led_tick = wio_millis();
      led_on = !led_on;
      vioSetSignal(vioLED0, led_on ? vioLEDon : vioLEDoff);
    }
    wio_delay_ms(10U);
  }
}
