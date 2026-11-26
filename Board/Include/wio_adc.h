/* SPDX-License-Identifier: Apache-2.0 */
/* Polled, single-conversion ADC support for the two onboard analog sensors. */
#ifndef WIO_ADC_H_
#define WIO_ADC_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIO_ADC_MIC_CHANNEL   12U
#define WIO_ADC_LIGHT_CHANNEL 15U
#define WIO_ADC_MAX_VALUE     4095U

/* Requires wio_sdk_init(). Owns ADC1, uses factory calibration, a 1.5 MHz
 * ADC clock (48 MHz GCLK0 / 32) and VDDANA as the reference. No ADC interrupts
 * or DMA are used. Only PD01 (light) and PC30 (microphone) are configured.
 */
bool wio_adc_init(void);

/* Only the two WIO_ADC_*_CHANNEL inputs are accepted. On success writes a
 * right-aligned 12-bit raw value; on failure leaves *value unchanged.
 * Calls are bounded and not reentrant. The readings are not lux or dB SPL.
 * This low-rate API is not an audio capture driver.
 */
bool wio_adc_read(uint8_t channel, uint16_t *value);
bool wio_adc_read_light(uint16_t *value);
bool wio_adc_read_microphone(uint16_t *value);

#ifdef __cplusplus
}
#endif
#endif
