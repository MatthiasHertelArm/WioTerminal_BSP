/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WIO_LCD_H_
#define WIO_LCD_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIO_LCD_WIDTH   320U
#define WIO_LCD_HEIGHT  240U

#define WIO_LCD_BLACK   0x0000U
#define WIO_LCD_BLUE    0x001FU
#define WIO_LCD_GREEN   0x07E0U
#define WIO_LCD_RED     0xF800U
#define WIO_LCD_WHITE   0xFFFFU

/*
 * Blocking ILI9341 adapter over the asynchronous CMSIS Driver_SPI7.
 * Writes use 8 MHz; diagnostic reads use 4 MHz. These are below the
 * controller's documented 10 MHz write / 6.67 MHz read limits.
 *
 * Call wio_sdk_init() first. Call these functions from thread/main context with
 * interrupts enabled, and serialize all access to this interface. Driver_SPI7
 * belongs exclusively to the display while it is initialized. No framebuffer,
 * heap, RTOS, or application rendering library is required.
 *
 * A true result means only that the host SPI transfers completed. SPI has no
 * acknowledgement, so it does NOT prove that an LCD is connected or that the
 * expected image is visible. Confirm the test pattern on the physical panel.
 * A transport error turns off the backlight and requires wio_lcd_init() again.
 */
bool wio_lcd_init(void);
bool wio_lcd_fill(uint16_t rgb565);

/* Rectangles must be nonempty and entirely inside the 320 x 240 display. */
bool wio_lcd_fill_rect(uint16_t x, uint16_t y,
                       uint16_t width, uint16_t height, uint16_t rgb565);

/*
 * Pixels are native-endian RGB565 values in contiguous row-major order, with
 * no row padding. The caller supplies at least width * height values; the
 * buffer is no longer used after this function returns. NULL is rejected.
 */
bool wio_lcd_write_rect(uint16_t x, uint16_t y,
                        uint16_t width, uint16_t height,
                        const uint16_t *pixels);

/*
 * Read the controller's diagnostic ID. A successful transfer may return zero
 * or all ones when the panel does not provide readable MISO; that is not an
 * identification pass. The output is changed only on transport success.
 */
bool wio_lcd_read_id(uint32_t *id);

/* Enabling the backlight has no effect before a successful initialization. */
void wio_lcd_backlight(bool on);

#ifdef __cplusplus
}
#endif

#endif /* WIO_LCD_H_ */
