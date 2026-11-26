/* SPDX-License-Identifier: Apache-2.0 */
#include "wio_lcd.h"

#include <stddef.h>

#include "Driver_SPI.h"
#include "wio_sdk.h"
#include "wio_terminal.h"

extern ARM_DRIVER_SPI Driver_SPI7;

/* ILI9341 data sheet p.243: write cycle >= 100 ns, read cycle >= 150 ns. */
#define LCD_WRITE_HZ             8000000U
#define LCD_READ_HZ              4000000U
#define LCD_TRANSFER_TIMEOUT_MS      100U
#define LCD_TRANSFER_SPIN_LIMIT   400000U
#define LCD_BUFFER_BYTES             128U

/*
 * ILI9341 data sheet, https://cdn-shop.adafruit.com/datasheets/ILI9341.pdf.
 * Commands: pp.90, 101, 109-114, 127, 134, 187. Reset timing: p.230.
 * Documentation-tool id: web/cdn-shop-adafruit-com/24eb8e36cf4d.
 */
#define ILI9341_SWRESET  0x01U
#define ILI9341_SLPOUT   0x11U
#define ILI9341_DISPON   0x29U
#define ILI9341_CASET    0x2AU
#define ILI9341_PASET    0x2BU
#define ILI9341_RAMWR    0x2CU
#define ILI9341_MADCTL   0x36U
#define ILI9341_COLMOD   0x3AU
#define ILI9341_RDID4    0xD3U

static bool lcd_ready;

/*
 * Driver_SPI7 is asynchronous. Use persistent staging storage for every
 * transaction, including command bytes, so a failed cancellation can never
 * leave the driver holding an automatic/local buffer after an error return.
 * This interface is deliberately non-reentrant; callers serialize access.
 */
static uint8_t lcd_buffer[LCD_BUFFER_BYTES];

static bool lcd_thread_context(void) {
  return (__get_IPSR() == 0U) && (__get_PRIMASK() == 0U) &&
         (__get_BASEPRI() == 0U) && (__get_FAULTMASK() == 0U);
}

static void lcd_abort(void) {
  if ((Driver_SPI7.Control(ARM_SPI_ABORT_TRANSFER, 0U) != ARM_DRIVER_OK) ||
      (Driver_SPI7.GetStatus().busy != 0U)) {
    (void)Driver_SPI7.PowerControl(ARM_POWER_OFF);
    (void)Driver_SPI7.Uninitialize();
  }
}

static bool lcd_fail(void) {
  lcd_abort();
  lcd_ready = false;
  wio_gpio_write(WIO_PIN_LCD_CS, 1U);
  wio_gpio_write(WIO_PIN_LCD_DC, 1U);
  wio_gpio_write(WIO_PIN_LCD_BACKLIGHT, 0U);
  return false;
}

static bool lcd_wait_transfer(uint32_t expected_count) {
  const uint32_t started = wio_millis();

  /* The spin bound still terminates if the application's timebase stops. */
  for (uint32_t spin = 0U; spin < LCD_TRANSFER_SPIN_LIMIT; ++spin) {
    const ARM_SPI_STATUS status = Driver_SPI7.GetStatus();

    if ((status.data_lost != 0U) || (status.mode_fault != 0U)) {
      break;
    }
    if (status.busy == 0U) {
      if (Driver_SPI7.GetDataCount() == expected_count) {
        return true;
      }
      break;
    }
    if ((uint32_t)(wio_millis() - started) >= LCD_TRANSFER_TIMEOUT_MS) {
      break;
    }
  }

  lcd_abort();
  return false;
}

static bool lcd_send(const uint8_t *bytes, uint32_t count) {
  if ((bytes == NULL) || (count == 0U) || (count > LCD_BUFFER_BYTES)) {
    return false;
  }
  if (Driver_SPI7.GetStatus().busy != 0U) {
    return false;
  }
  for (uint32_t i = 0U; i < count; ++i) {
    lcd_buffer[i] = bytes[i];
  }

  if (Driver_SPI7.Send(lcd_buffer, count) != ARM_DRIVER_OK) {
    return false;
  }
  return lcd_wait_transfer(count);
}

static bool lcd_receive(uint8_t *bytes, uint32_t count) {
  if ((bytes == NULL) || (count == 0U) || (count > LCD_BUFFER_BYTES)) {
    return false;
  }
  if (Driver_SPI7.Receive(lcd_buffer, count) != ARM_DRIVER_OK) {
    return false;
  }
  if (!lcd_wait_transfer(count)) {
    return false;
  }

  for (uint32_t i = 0U; i < count; ++i) {
    bytes[i] = lcd_buffer[i];
  }
  return true;
}

static bool lcd_valid_rect(uint16_t x, uint16_t y,
                           uint16_t width, uint16_t height) {
  return (width != 0U) && (height != 0U) &&
         (x < WIO_LCD_WIDTH) && (y < WIO_LCD_HEIGHT) &&
         (width <= (WIO_LCD_WIDTH - x)) &&
         (height <= (WIO_LCD_HEIGHT - y));
}

/* Leave CS asserted for parameters/pixels, with D/C in the data state. */
static bool lcd_begin_command(uint8_t command) {
  wio_gpio_write(WIO_PIN_LCD_CS, 0U);
  wio_gpio_write(WIO_PIN_LCD_DC, 0U);
  wio_delay_us(1U);
  if (!lcd_send(&command, 1U)) {
    return false;
  }
  wio_gpio_write(WIO_PIN_LCD_DC, 1U);
  wio_delay_us(1U);
  return true;
}

static void lcd_end_transaction(void) {
  /* lcd_send/receive have waited for the final physical TXC, not just DRE. */
  wio_delay_us(1U);
  wio_gpio_write(WIO_PIN_LCD_CS, 1U);
  wio_gpio_write(WIO_PIN_LCD_DC, 1U);
  wio_delay_us(1U);
}

static bool lcd_command(uint8_t command,
                        const uint8_t *parameters, uint32_t count) {
  bool success = lcd_begin_command(command);
  if (success && (count != 0U)) {
    success = lcd_send(parameters, count);
  }
  lcd_end_transaction();
  return success;
}

static bool lcd_begin_rect(uint16_t x, uint16_t y,
                           uint16_t width, uint16_t height) {
  const uint16_t right = (uint16_t)(x + width - 1U);
  const uint16_t bottom = (uint16_t)(y + height - 1U);
  const uint8_t columns[4] = {
    (uint8_t)(x >> 8), (uint8_t)x,
    (uint8_t)(right >> 8), (uint8_t)right
  };
  const uint8_t rows[4] = {
    (uint8_t)(y >> 8), (uint8_t)y,
    (uint8_t)(bottom >> 8), (uint8_t)bottom
  };

  return lcd_command(ILI9341_CASET, columns, sizeof(columns)) &&
         lcd_command(ILI9341_PASET, rows, sizeof(rows)) &&
         lcd_begin_command(ILI9341_RAMWR);
}

static bool lcd_draw_rect(uint16_t x, uint16_t y,
                          uint16_t width, uint16_t height,
                          const uint16_t *pixels, uint16_t solid_color) {
  if (!lcd_ready || !lcd_thread_context() ||
      !lcd_valid_rect(x, y, width, height)) {
    return false;
  }
  if (!lcd_begin_rect(x, y, width, height)) {
    return lcd_fail();
  }

  uint8_t block[LCD_BUFFER_BYTES];
  uint32_t remaining = (uint32_t)width * height;
  uint32_t offset = 0U;

  while (remaining != 0U) {
    const uint32_t count = (remaining < LCD_BUFFER_BYTES / 2U) ?
                           remaining : LCD_BUFFER_BYTES / 2U;
    for (uint32_t i = 0U; i < count; ++i) {
      const uint16_t pixel = (pixels != NULL) ? pixels[offset + i] : solid_color;
      /* p.63: RGB565 is sent high byte first, independent of host endianness. */
      block[2U * i] = (uint8_t)(pixel >> 8);
      block[2U * i + 1U] = (uint8_t)pixel;
    }
    if (!lcd_send(block, 2U * count)) {
      return lcd_fail();
    }
    offset += count;
    remaining -= count;
  }

  lcd_end_transaction();
  return true;
}

bool wio_lcd_init(void) {
  const uint8_t pixel_format = 0x55U; /* 16-bit RGB/MCU interfaces, p.134. */
  const uint8_t landscape = 0x28U;    /* MV + BGR, p.127. */

  if (!lcd_thread_context() || (Driver_SPI7.GetStatus().busy != 0U)) {
    return false;
  }

  lcd_ready = false;
  wio_gpio_output(WIO_PIN_LCD_BACKLIGHT, 0U);
  wio_gpio_output(WIO_PIN_LCD_CS, 1U);
  wio_gpio_output(WIO_PIN_LCD_DC, 1U);
  wio_gpio_output(WIO_PIN_LCD_RESET, 1U);

  if (Driver_SPI7.Initialize(NULL) != ARM_DRIVER_OK) {
    return false;
  }
  if (Driver_SPI7.PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK) {
    goto initialization_failed;
  }
  if (Driver_SPI7.Control(ARM_SPI_MODE_MASTER | ARM_SPI_CPOL0_CPHA0 |
                          ARM_SPI_DATA_BITS(8U) | ARM_SPI_MSB_LSB |
                          ARM_SPI_SS_MASTER_UNUSED,
                          LCD_WRITE_HZ) != ARM_DRIVER_OK) {
    goto initialization_failed;
  }
  if (Driver_SPI7.Control(ARM_SPI_SET_DEFAULT_TX_VALUE, 0xFFU) != ARM_DRIVER_OK) {
    goto initialization_failed;
  }
  /* A previous software-SS configuration may have released CS in Control. */
  wio_gpio_output(WIO_PIN_LCD_CS, 1U);

  /* Hardware reset loads the panel supplier's nonvolatile drive defaults. */
  wio_delay_ms(5U);
  wio_gpio_write(WIO_PIN_LCD_RESET, 0U);
  wio_delay_ms(20U);
  wio_gpio_write(WIO_PIN_LCD_RESET, 1U);
  wio_delay_ms(120U);
  if (!lcd_command(ILI9341_SWRESET, NULL, 0U)) {
    goto initialization_failed;
  }
  wio_delay_ms(120U);
  if (!lcd_command(ILI9341_SLPOUT, NULL, 0U)) {
    goto initialization_failed;
  }
  wio_delay_ms(120U);

  /* Keep factory analog/gamma settings; do not guess panel drive voltages. */
  if (!lcd_command(ILI9341_COLMOD, &pixel_format, 1U) ||
      !lcd_command(ILI9341_MADCTL, &landscape, 1U)) {
    goto initialization_failed;
  }

  lcd_ready = true;
  if (!wio_lcd_fill(WIO_LCD_BLACK) ||
      !lcd_command(ILI9341_DISPON, NULL, 0U)) {
    goto initialization_failed;
  }
  wio_delay_ms(20U);
  wio_lcd_backlight(true);
  return true;

initialization_failed:
  (void)lcd_fail();
  (void)Driver_SPI7.PowerControl(ARM_POWER_OFF);
  (void)Driver_SPI7.Uninitialize();
  return false;
}

bool wio_lcd_fill(uint16_t rgb565) {
  return wio_lcd_fill_rect(0U, 0U, WIO_LCD_WIDTH, WIO_LCD_HEIGHT, rgb565);
}

bool wio_lcd_fill_rect(uint16_t x, uint16_t y,
                       uint16_t width, uint16_t height, uint16_t rgb565) {
  return lcd_draw_rect(x, y, width, height, NULL, rgb565);
}

bool wio_lcd_write_rect(uint16_t x, uint16_t y,
                        uint16_t width, uint16_t height,
                        const uint16_t *pixels) {
  if (pixels == NULL) {
    return false;
  }
  return lcd_draw_rect(x, y, width, height, pixels, 0U);
}

bool wio_lcd_read_id(uint32_t *id) {
  uint8_t response[4];

  if (!lcd_ready || !lcd_thread_context() || (id == NULL)) {
    return false;
  }
  if (Driver_SPI7.Control(ARM_SPI_SET_BUS_SPEED, LCD_READ_HZ) != ARM_DRIVER_OK) {
    return lcd_fail();
  }
  const bool success = lcd_begin_command(ILI9341_RDID4) &&
                       lcd_receive(response, sizeof(response));
  lcd_end_transaction();
  if (!success ||
      (Driver_SPI7.Control(ARM_SPI_SET_BUS_SPEED, LCD_WRITE_HZ) != ARM_DRIVER_OK)) {
    return lcd_fail();
  }

  /* p.187: dummy byte, version, model high, model low. No ID pass/fail test. */
  *id = ((uint32_t)response[1] << 16) |
        ((uint32_t)response[2] << 8) | response[3];
  return true;
}

void wio_lcd_backlight(bool on) {
  wio_gpio_output(WIO_PIN_LCD_BACKLIGHT, (on && lcd_ready) ? 1U : 0U);
}
