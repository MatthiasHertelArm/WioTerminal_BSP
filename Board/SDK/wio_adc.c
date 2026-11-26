/* SPDX-License-Identifier: Apache-2.0 */
/* SAM D5x/E5x DS60001507N: calibration pp.54-55/1531; ADC pp.1483-1531.
 * Independently implemented using the Microchip DFP register definitions.
 */
#include <stddef.h>
#include "wio_adc.h"
#include "wio_sdk.h"
#include "wio_terminal.h"

#define WIO_ADC1_GCLK_ID        41U  /* DS60001507N, Table 14-9 */
#define WIO_ADC_TIMEOUT_MS     10U
#define WIO_ADC_POLL_BUDGET    1000000U
#define WIO_ADC_CALIB_ADDRESS  0x00800080UL

static bool adc_ready;

static bool wait_sync(void) {
  const uint32_t start = wio_millis();
  uint32_t budget = WIO_ADC_POLL_BUDGET;

  while (ADC1_REGS->ADC_SYNCBUSY != 0U) {
    if ((--budget == 0U) ||
        ((uint32_t)(wio_millis() - start) >= WIO_ADC_TIMEOUT_MS)) {
      adc_ready = false;
      return false;
    }
  }
  return true;
}

static void analog_pin(uint8_t pin) {
  port_group_registers_t *port = wio_port(pin);
  const uint32_t number = WIO_PIN_NUMBER(pin);

  port->PORT_DIRCLR = 1UL << number;
  port->PORT_PINCFG[number] = 0U;  /* No digital buffer or pull resistor. */
  wio_gpio_function(pin, 1U);      /* Peripheral function B: ADC input. */
}

bool wio_adc_init(void) {
  uint32_t calibration;

  adc_ready = false;
  if (SystemCoreClock != 48000000U) {
    return false;
  }
  MCLK_REGS->MCLK_APBDMASK |= MCLK_APBDMASK_ADC1_Msk;
  if (!wio_gclk_enable(WIO_ADC1_GCLK_ID)) {
    return false;
  }
  ADC1_REGS->ADC_CTRLA &= (uint16_t)~ADC_CTRLA_ENABLE_Msk;
  if (!wait_sync()) {
    return false;
  }
  ADC1_REGS->ADC_CTRLA = ADC_CTRLA_SWRST_Msk;
  if (!wait_sync() || ((ADC1_REGS->ADC_CTRLA & ADC_CTRLA_SWRST_Msk) != 0U)) {
    return false;
  }

  /* Copy the read-only factory calibration values, never write the fuse row.
   * ADC1 fields in SW0: BIASCOMP[18:16], BIASREFBUF[21:19], BIASR2R[24:22].
   */
  calibration = *(const volatile uint32_t *)(uintptr_t)WIO_ADC_CALIB_ADDRESS;
  ADC1_REGS->ADC_CALIB = (uint16_t)(
      ADC_CALIB_BIASCOMP((calibration >> 16) & 7U) |
      ADC_CALIB_BIASREFBUF((calibration >> 19) & 7U) |
      ADC_CALIB_BIASR2R((calibration >> 22) & 7U));

  analog_pin(WIO_PIN_LIGHT);
  analog_pin(WIO_PIN_MIC);
  ADC1_REGS->ADC_CTRLA = ADC_CTRLA_PRESCALER_DIV32;
  ADC1_REGS->ADC_CTRLB = ADC_CTRLB_RESSEL_12BIT;
  if (!wait_sync()) {
    return false;
  }
  ADC1_REGS->ADC_REFCTRL = ADC_REFCTRL_REFSEL_INTVCC1;
  if (!wait_sync()) {
    return false;
  }
  ADC1_REGS->ADC_AVGCTRL = 0U;  /* One conversion, no averaging. */
  if (!wait_sync()) {
    return false;
  }
  ADC1_REGS->ADC_SAMPCTRL = ADC_SAMPCTRL_SAMPLEN(31U);
  if (!wait_sync()) {
    return false;
  }
  ADC1_REGS->ADC_INPUTCTRL = ADC_INPUTCTRL_MUXNEG_GND |
                             ADC_INPUTCTRL_MUXPOS(WIO_ADC_LIGHT_CHANNEL);
  if (!wait_sync()) {
    return false;
  }
  ADC1_REGS->ADC_INTENCLR = ADC_INTENCLR_Msk;
  ADC1_REGS->ADC_INTFLAG = ADC_INTFLAG_Msk;
  ADC1_REGS->ADC_CTRLA |= ADC_CTRLA_ENABLE_Msk;
  if (!wait_sync()) {
    return false;
  }
  adc_ready = true;
  return true;
}

static bool convert(uint16_t *value) {
  uint32_t start;
  uint32_t budget = WIO_ADC_POLL_BUDGET;

  ADC1_REGS->ADC_INTFLAG = ADC_INTFLAG_RESRDY_Msk | ADC_INTFLAG_OVERRUN_Msk;
  ADC1_REGS->ADC_SWTRIG = ADC_SWTRIG_START_Msk;
  if (!wait_sync()) {
    return false;
  }
  start = wio_millis();
  while ((ADC1_REGS->ADC_INTFLAG & ADC_INTFLAG_RESRDY_Msk) == 0U) {
    if ((--budget == 0U) ||
        ((uint32_t)(wio_millis() - start) >= WIO_ADC_TIMEOUT_MS)) {
      adc_ready = false;  /* An explicit re-init is needed after a timeout. */
      return false;
    }
  }
  if ((ADC1_REGS->ADC_INTFLAG & ADC_INTFLAG_OVERRUN_Msk) != 0U) {
    adc_ready = false;
    return false;
  }
  *value = (uint16_t)(ADC1_REGS->ADC_RESULT & WIO_ADC_MAX_VALUE);
  return true;
}

bool wio_adc_read(uint8_t channel, uint16_t *value) {
  uint16_t discarded;
  uint16_t result;

  if (!adc_ready || (value == NULL) ||
      ((channel != WIO_ADC_LIGHT_CHANNEL) && (channel != WIO_ADC_MIC_CHANNEL))) {
    return false;
  }
  ADC1_REGS->ADC_INPUTCTRL = ADC_INPUTCTRL_MUXNEG_GND | ADC_INPUTCTRL_MUXPOS(channel);
  if (!wait_sync()) {
    return false;
  }
  /* Discard one conversion after changing the input channel. */
  if (!convert(&discarded) || !convert(&result)) {
    return false;
  }
  *value = result;
  return true;
}

bool wio_adc_read_light(uint16_t *value) {
  return wio_adc_read(WIO_ADC_LIGHT_CHANNEL, value);
}

bool wio_adc_read_microphone(uint16_t *value) {
  return wio_adc_read(WIO_ADC_MIC_CHANNEL, value);
}
