/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WIO_HARDWARE_TEST_H_
#define WIO_HARDWARE_TEST_H_
#include <stdint.h>

#define HARDWARE_TEST_REPORT_VERSION 3U
#define HARDWARE_TEST_RESULT_UNSET UINT32_MAX

typedef enum {
  HW_NOT_RUN = 0,
  HW_PASS = 1,
  HW_FAIL = 2,
  HW_MANUAL = 3,
  HW_SKIPPED = 4
} hw_result_t;

/* Bits in sdk_arguments_run and sdk_arguments_failed. */
enum {
  HW_ARGUMENT_I2C = 1U << 0,
  HW_ARGUMENT_ADC = 1U << 1,
  HW_ARGUMENT_LCD = 1U << 2,
  HW_ARGUMENT_ALL = HW_ARGUMENT_I2C | HW_ARGUMENT_ADC | HW_ARGUMENT_LCD
};

typedef struct {
  hw_result_t clock;
  hw_result_t bootloader_protection;
  hw_result_t driver_api;
  hw_result_t sercom0_ownership;
  hw_result_t led_gpio;
  hw_result_t led_visual;
  hw_result_t buttons;
  hw_result_t accelerometer_id;
  hw_result_t accelerometer_samples;
  hw_result_t light_adc;
  hw_result_t microphone_adc;
  hw_result_t lcd_spi;
  hw_result_t lcd_visual;
  hw_result_t sdk_arguments;
  hw_result_t flash_id;
  hw_result_t flash_read;
  hw_result_t sd_card;
  hw_result_t filesystem;
  hw_result_t filesystem_fixture;
  hw_result_t external_interfaces;
  hw_result_t radio;
  hw_result_t network_link;
  hw_result_t usb;
  hw_result_t buzzer;
  hw_result_t infrared;
} hardware_test_checks_t;

/* Results persist in RAM and can be inspected without a UART/USB stack. */
typedef struct {
  uint32_t magic;                    /* 0x57494F54 = WIOT */
  uint32_t version;
  uint32_t phase;                    /* 1 core, 2 API, 3 GPIO, 4 motion,
                                       5 ADC, 6 LCD, 7 flash, 8 SD/FAT,
                                       9 radio probe, 10 interactive */
  uint32_t completed;
  hardware_test_checks_t checks;
  uint32_t passed, failed, manual, skipped, not_run;
  uint32_t core_clock_hz, bootprot, elapsed_ms, heartbeat;
  uint32_t buttons;                 /* Current debounced active input levels. */
  uint32_t buttons_seen;            /* Any active levels, diagnostic only. */
  uint32_t buttons_released;        /* Debounced initial release observed. */
  uint32_t buttons_pressed;         /* Press observed after initial release. */
  uint32_t buttons_completed;       /* Release observed after qualified press. */
  uint32_t buzzer_requests;
  uint32_t sdk_arguments_run, sdk_arguments_failed;
  uint32_t i2c_events, i2c_completions, spi_events, spi_completions;
  uint32_t accel_address, accel_who_am_i, accel_samples;
  int32_t accel_x_mg, accel_y_mg, accel_z_mg;
  uint32_t accel_norm_squared;
  uint32_t light_min, light_max, light_mean, light_samples;
  uint32_t mic_min, mic_max, mic_mean, mic_samples;
  uint32_t lcd_id, lcd_transfer_count, flash_jedec_id, flash_sample_hash;
  uint32_t sd_detect_raw;
  uint32_t sd_result, sd_sector_size, sd_block_addressed, sd_sector0_hash;
  uint64_t sd_sector_count;
  uint32_t fs_result, fs_entries, fs_scan_capped, fs_fixture_bytes;
  /* FRESULT fields use HARDWARE_TEST_RESULT_UNSET until actually attempted. */
  uint32_t fs_fixture_hash, fs_fixture_result, fs_unmount_result;
  int32_t network_result, network_remote_result, network_remote_errno;
  uint32_t network_state, network_baud, network_echo_ok, network_rpc_count;
  uint32_t network_event_count;
  char network_firmware[16];
} hardware_test_results_t;

extern volatile hardware_test_results_t g_hw_test;
void hardware_test_checkpoint(void);
#endif
