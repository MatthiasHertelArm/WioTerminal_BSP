/******************************************************************************
 * @file     wio_terminal.h
 * @brief    Pin map and GPIO helpers for the Seeed Studio Wio Terminal
 * @version  V1.0.0
 * @date     28. October 2026
 ******************************************************************************/
/*
 * Copyright (c) 2026 Arm Limited (or its affiliates).
 * All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Pin assignment from the Seeed Studio Arduino core for the Wio Terminal
 * (Seeed-Studio/ArduinoCore-samd, variants/wio_terminal/variant.cpp). SERCOM pads
 * and peripheral functions (C = SERCOM, D = SERCOM-ALT, H = QSPI) as configured there.
 */

#ifndef WIO_TERMINAL_H_
#define WIO_TERMINAL_H_

#include <stdint.h>
#include "RTE_Components.h"
#include CMSIS_device_header

#ifdef __cplusplus
extern "C" {
#endif

/* Pin encoding: PORT group in bits 7..5, pin number in bits 4..0 */
#define WIO_PIN(group, pin)     ((uint8_t)(((group) << 5) | (pin)))
#define WIO_PIN_GROUP(p)        ((uint32_t)(p) >> 5)
#define WIO_PIN_NUMBER(p)       ((uint32_t)(p) & 0x1FU)

#define WIO_PORT_A              0U
#define WIO_PORT_B              1U
#define WIO_PORT_C              2U
#define WIO_PORT_D              3U

/* User LED, buttons, 5-way switch */
#define WIO_PIN_LED             WIO_PIN(WIO_PORT_A, 15)   /* Blue user LED, active high       */
#define WIO_PIN_KEY_A           WIO_PIN(WIO_PORT_C, 26)   /* Button A, active low             */
#define WIO_PIN_KEY_B           WIO_PIN(WIO_PORT_C, 27)   /* Button B, active low             */
#define WIO_PIN_KEY_C           WIO_PIN(WIO_PORT_C, 28)   /* Button C, active low             */
#define WIO_PIN_5S_UP           WIO_PIN(WIO_PORT_D, 20)   /* 5-way switch up, active low      */
#define WIO_PIN_5S_DOWN         WIO_PIN(WIO_PORT_D,  8)   /* 5-way switch down, active low    */
#define WIO_PIN_5S_LEFT         WIO_PIN(WIO_PORT_D, 12)   /* 5-way switch left, active low    */
#define WIO_PIN_5S_RIGHT        WIO_PIN(WIO_PORT_D,  9)   /* 5-way switch right, active low   */
#define WIO_PIN_5S_PRESS        WIO_PIN(WIO_PORT_D, 10)   /* 5-way switch press, active low   */

/* Buzzer, sensors, infrared */
#define WIO_PIN_BUZZER          WIO_PIN(WIO_PORT_D, 11)   /* Buzzer, TCC0 WO[4]               */
#define WIO_PIN_LIGHT           WIO_PIN(WIO_PORT_D,  1)   /* Light sensor, ADC1 AIN15         */
#define WIO_PIN_MIC             WIO_PIN(WIO_PORT_C, 30)   /* Microphone, ADC1 AIN12           */
#define WIO_PIN_IR              WIO_PIN(WIO_PORT_B, 31)   /* Infrared emitter, TC0 WO[1]      */

/* LCD (ILI9341) on SERCOM7, function D */
#define WIO_PIN_LCD_MISO        WIO_PIN(WIO_PORT_B, 18)   /* SERCOM7 PAD[2]                   */
#define WIO_PIN_LCD_MOSI        WIO_PIN(WIO_PORT_B, 19)   /* SERCOM7 PAD[3]                   */
#define WIO_PIN_LCD_SCK         WIO_PIN(WIO_PORT_B, 20)   /* SERCOM7 PAD[1]                   */
#define WIO_PIN_LCD_CS          WIO_PIN(WIO_PORT_B, 21)   /* SERCOM7 PAD[0]                   */
#define WIO_PIN_LCD_DC          WIO_PIN(WIO_PORT_C,  6)   /* Data/command                     */
#define WIO_PIN_LCD_RESET       WIO_PIN(WIO_PORT_C,  7)   /* Reset, active low                */
#define WIO_PIN_LCD_BACKLIGHT   WIO_PIN(WIO_PORT_C,  5)   /* Backlight                        */
#define WIO_PIN_LCD_XL          WIO_PIN(WIO_PORT_C, 10)   /* Resistive touch X left           */
#define WIO_PIN_LCD_YU          WIO_PIN(WIO_PORT_C, 11)   /* Resistive touch Y up             */
#define WIO_PIN_LCD_XR          WIO_PIN(WIO_PORT_C, 12)   /* Resistive touch X right          */
#define WIO_PIN_LCD_YD          WIO_PIN(WIO_PORT_C, 13)   /* Resistive touch Y down           */

/* microSD card on SERCOM6, function C */
#define WIO_PIN_SD_MOSI         WIO_PIN(WIO_PORT_C, 16)   /* SERCOM6 PAD[0]                   */
#define WIO_PIN_SD_SCK          WIO_PIN(WIO_PORT_C, 17)   /* SERCOM6 PAD[1]                   */
#define WIO_PIN_SD_MISO         WIO_PIN(WIO_PORT_C, 18)   /* SERCOM6 PAD[2]                   */
#define WIO_PIN_SD_CS           WIO_PIN(WIO_PORT_C, 19)   /* SERCOM6 PAD[3]                   */
#define WIO_PIN_SD_DET          WIO_PIN(WIO_PORT_D, 21)   /* Card detect                      */

/* LIS3DH accelerometer on SERCOM4 (I2C), function D */
#define WIO_PIN_ACC_SDA         WIO_PIN(WIO_PORT_A, 13)   /* SERCOM4 PAD[0]                   */
#define WIO_PIN_ACC_SCL         WIO_PIN(WIO_PORT_A, 12)   /* SERCOM4 PAD[1]                   */
#define WIO_PIN_ACC_INT1        WIO_PIN(WIO_PORT_C, 21)   /* Interrupt 1                      */

/* Grove I2C on SERCOM3, function D */
#define WIO_PIN_I2C_SDA         WIO_PIN(WIO_PORT_A, 17)   /* SERCOM3 PAD[0]                   */
#define WIO_PIN_I2C_SCL         WIO_PIN(WIO_PORT_A, 16)   /* SERCOM3 PAD[1]                   */

/* UART on the 40-pin header (pins 8 and 10) on SERCOM2, function C */
#define WIO_PIN_UART_TX         WIO_PIN(WIO_PORT_B, 26)   /* SERCOM2 PAD[0]                   */
#define WIO_PIN_UART_RX         WIO_PIN(WIO_PORT_B, 27)   /* SERCOM2 PAD[1]                   */

/* SPI on the 40-pin header on SERCOM5, function D */
#define WIO_PIN_SPI_MOSI        WIO_PIN(WIO_PORT_B,  2)   /* SERCOM5 PAD[0]                   */
#define WIO_PIN_SPI_SCK         WIO_PIN(WIO_PORT_B,  3)   /* SERCOM5 PAD[1]                   */
#define WIO_PIN_SPI_MISO        WIO_PIN(WIO_PORT_B,  0)   /* SERCOM5 PAD[2]                   */
#define WIO_PIN_SPI_CS          WIO_PIN(WIO_PORT_B,  1)   /* SERCOM5 PAD[3]                   */

/* RTL8720DN wireless module: UART on SERCOM1 (function C), SPI on SERCOM0 (function C) */
#define WIO_PIN_RTL_UART_TX     WIO_PIN(WIO_PORT_C, 22)   /* SERCOM1 PAD[0]                   */
#define WIO_PIN_RTL_UART_RX     WIO_PIN(WIO_PORT_C, 23)   /* SERCOM1 PAD[1]                   */
#define WIO_PIN_RTL_SPI_MOSI    WIO_PIN(WIO_PORT_B, 24)   /* SERCOM0 PAD[0]                   */
#define WIO_PIN_RTL_SPI_SCK     WIO_PIN(WIO_PORT_B, 25)   /* SERCOM0 PAD[1]                   */
#define WIO_PIN_RTL_SPI_MISO    WIO_PIN(WIO_PORT_C, 24)   /* SERCOM0 PAD[2]                   */
#define WIO_PIN_RTL_SPI_CS      WIO_PIN(WIO_PORT_C, 25)   /* SERCOM0 PAD[3]                   */
/* Seeed eRPC uses these same pads as SERCOM0 UART, not the SERCOM1 loader UART.
 * ArduinoCore-samd 2e6b174f81b988964ce3811f5b5e39297c2c9c3f variant pins 84/85;
 * rpcUnified f0570f481d6f65c2e86c147f36d2b994d7bea645: RX pad2, TX pad0.
 */
#define WIO_PIN_RTL_ERPC_TX     WIO_PIN_RTL_SPI_MOSI
#define WIO_PIN_RTL_ERPC_RX     WIO_PIN_RTL_SPI_MISO
#define WIO_PIN_RTL_CHIP_PU     WIO_PIN(WIO_PORT_A, 18)   /* Chip enable                      */
#define WIO_PIN_RTL_SYNC        WIO_PIN(WIO_PORT_A, 19)   /* RTL8720DN PA13 / SPI sync        */
#define WIO_PIN_RTL_IRQ         WIO_PIN(WIO_PORT_C, 20)   /* RTL8720DN PA12 / SPI interrupt   */

/* QSPI Flash W25Q32JV, function H */
#define WIO_PIN_QSPI_IO0        WIO_PIN(WIO_PORT_A,  8)
#define WIO_PIN_QSPI_IO1        WIO_PIN(WIO_PORT_A,  9)
#define WIO_PIN_QSPI_IO2        WIO_PIN(WIO_PORT_A, 10)
#define WIO_PIN_QSPI_IO3        WIO_PIN(WIO_PORT_A, 11)
#define WIO_PIN_QSPI_SCK        WIO_PIN(WIO_PORT_B, 10)
#define WIO_PIN_QSPI_CS         WIO_PIN(WIO_PORT_B, 11)

/* USB, power outputs, debug */
#define WIO_PIN_USB_DM          WIO_PIN(WIO_PORT_A, 24)
#define WIO_PIN_USB_DP          WIO_PIN(WIO_PORT_A, 25)
#define WIO_PIN_USB_HOST_EN     WIO_PIN(WIO_PORT_A, 27)   /* USB host power enable            */
#define WIO_PIN_OUT_5V_EN       WIO_PIN(WIO_PORT_C, 14)   /* 5 V output control (header)      */
#define WIO_PIN_OUT_3V3_EN      WIO_PIN(WIO_PORT_C, 15)   /* 3.3 V output control (header)    */
#define WIO_PIN_SWCLK           WIO_PIN(WIO_PORT_A, 30)
#define WIO_PIN_SWDIO           WIO_PIN(WIO_PORT_A, 31)
#define WIO_PIN_SWO             WIO_PIN(WIO_PORT_B, 30)

/* PORT group registers of a pin */
static inline port_group_registers_t *wio_port (uint8_t pin) {
  return &PORT_REGS->GROUP[WIO_PIN_GROUP(pin)];
}

/* Configure a pin as push-pull output with an initial level */
static inline void wio_gpio_output (uint8_t pin, uint32_t level) {
  port_group_registers_t *port = wio_port(pin);
  uint32_t mask = 1UL << WIO_PIN_NUMBER(pin);

  if (level != 0U) {
    port->PORT_OUTSET = mask;
  } else {
    port->PORT_OUTCLR = mask;
  }
  port->PORT_PINCFG[WIO_PIN_NUMBER(pin)] = 0U;
  port->PORT_DIRSET = mask;
}

/* Configure a pin as input; pull: 0 = none, 1 = pull-up, -1 = pull-down */
static inline void wio_gpio_input (uint8_t pin, int32_t pull) {
  port_group_registers_t *port = wio_port(pin);
  uint32_t mask = 1UL << WIO_PIN_NUMBER(pin);

  port->PORT_DIRCLR = mask;
  if (pull > 0) {
    port->PORT_OUTSET = mask;
  } else {
    port->PORT_OUTCLR = mask;
  }
  port->PORT_PINCFG[WIO_PIN_NUMBER(pin)] = PORT_PINCFG_INEN_Msk |
                                           ((pull != 0) ? PORT_PINCFG_PULLEN_Msk : 0U);
}

/* Assign a peripheral function (0 = A .. 13 = N) to a pin */
static inline void wio_gpio_function (uint8_t pin, uint32_t function) {
  port_group_registers_t *port = wio_port(pin);
  uint32_t n     = WIO_PIN_NUMBER(pin);
  uint8_t  pmux  = port->PORT_PMUX[n >> 1];

  if ((n & 1U) != 0U) {
    pmux = (uint8_t)((pmux & 0x0FU) | ((function & 0x0FU) << 4));
  } else {
    pmux = (uint8_t)((pmux & 0xF0U) |  (function & 0x0FU));
  }
  port->PORT_PMUX[n >> 1]  = pmux;
  port->PORT_PINCFG[n]    |= PORT_PINCFG_PMUXEN_Msk;
}

/* Drive an output pin */
static inline void wio_gpio_write (uint8_t pin, uint32_t level) {
  if (level != 0U) {
    wio_port(pin)->PORT_OUTSET = 1UL << WIO_PIN_NUMBER(pin);
  } else {
    wio_port(pin)->PORT_OUTCLR = 1UL << WIO_PIN_NUMBER(pin);
  }
}

/* Toggle an output pin */
static inline void wio_gpio_toggle (uint8_t pin) {
  wio_port(pin)->PORT_OUTTGL = 1UL << WIO_PIN_NUMBER(pin);
}

/* Read the level of an input pin (0 or 1) */
static inline uint32_t wio_gpio_read (uint8_t pin) {
  return (wio_port(pin)->PORT_IN >> WIO_PIN_NUMBER(pin)) & 1U;
}

#ifdef __cplusplus
}
#endif

#endif /* WIO_TERMINAL_H_ */
