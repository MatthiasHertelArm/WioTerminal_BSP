/* Copyright (c) 2026 Arm Limited (or its affiliates).
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef WIO_SDK_H_
#define WIO_SDK_H_

#include <stdbool.h>
#include <stdint.h>
#include "wio_terminal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bare-metal board services for the nominal 48 MHz DFLL/GCLK0 configuration.
 * Checks the actual GCLK0 source, generator/CPU dividers and DFLL readiness;
 * rejects other clock setups instead of rewriting the application's CPU clock.
 * Owns SysTick and DWT CYCCNT; succeeds only if the cycle counter advances.
 * Call before using any other SDK service. Do not later change these clocks,
 * the SysTick configuration, or the DWT counter while using the SDK.
 * Does not enable header/Grove power, USB host power, or the radio.
 * Applications with an RTOS must supply their own timebase integration.
 */
bool wio_sdk_init(void);
uint32_t wio_millis(void);
void wio_delay_ms(uint32_t ms);
void wio_delay_us(uint32_t us);
/* Route one peripheral channel to GCLK0 with bounded CHEN synchronization.
 * Requires the supported hardware clock configuration; disables a different
 * source before switching. A locked channel is accepted only if already correct.
 * The caller must own the peripheral and keep it disabled during reconfiguration.
 */
bool wio_gclk_enable(uint32_t channel);

/* Optional bounded, blocking audible test; never called automatically.
 * 40..10000 Hz, 1..1000 ms. Ends with the buzzer output low.
 */
bool wio_buzzer_tone(uint32_t frequency_hz, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
#endif
