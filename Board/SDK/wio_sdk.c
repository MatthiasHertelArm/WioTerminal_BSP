/* Copyright (c) 2026 Arm Limited (or its affiliates).
 * SPDX-License-Identifier: Apache-2.0
 * Timing services adapted from the Wio Recorder board services.
 */
#include "wio_sdk.h"

#define WIO_CORE_CLOCK_HZ   48000000U
#define WIO_CLOCK_GUARD     100000U
#define WIO_DWT_PROBE_GUARD 1024U

static volatile uint32_t ticks_ms;
static bool initialized;

static bool clock_supported(void) {
  /* The DFP's SystemCoreClockUpdate() assigns a constant; it does not inspect
   * hardware. Verify the actual source and dividers instead of trusting it.
   * DS60001507N pp.150-154, 169, 740, 744. Do not change the CPU clock here.
   */
  const uint32_t generator = GCLK_REGS->GCLK_GENCTRL[0];
  const uint32_t divider = (generator & GCLK_GENCTRL_DIV_Msk) >> GCLK_GENCTRL_DIV_Pos;

  return ((GCLK_REGS->GCLK_SYNCBUSY & (1UL << GCLK_SYNCBUSY_GENCTRL_Pos)) == 0U) &&
         ((generator & GCLK_GENCTRL_GENEN_Msk) != 0U) &&
         ((generator & GCLK_GENCTRL_SRC_Msk) == GCLK_GENCTRL_SRC_DFLL) &&
         ((generator & GCLK_GENCTRL_DIVSEL_Msk) == 0U) && (divider <= 1U) &&
         ((MCLK_REGS->MCLK_CPUDIV & MCLK_CPUDIV_DIV_Msk) == 1U) &&
         ((OSCCTRL_REGS->OSCCTRL_DFLLCTRLA & OSCCTRL_DFLLCTRLA_ENABLE_Msk) != 0U) &&
         ((OSCCTRL_REGS->OSCCTRL_STATUS & OSCCTRL_STATUS_DFLLRDY_Msk) != 0U);
}

static bool cycle_counter_running(void) {
  /* DWT_CTRL.NOCYCCNT reports an absent cycle counter. An enable bit alone
   * does not prove that CYCCNT advances, so test it with a finite read loop.
   * Armv7-M Architecture Reference Manual DDI0403E.e, pp.738, 740-741.
   */
  if (((CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk) == 0U) ||
      ((DWT->CTRL & (DWT_CTRL_NOCYCCNT_Msk | DWT_CTRL_CYCCNTENA_Msk)) !=
        DWT_CTRL_CYCCNTENA_Msk)) {
    return false;
  }
  const uint32_t start = DWT->CYCCNT;
  for (uint32_t guard = WIO_DWT_PROBE_GUARD; guard != 0U; --guard) {
    __NOP();
    if (DWT->CYCCNT != start) {
      return true;
    }
  }
  return false;
}

void SysTick_Handler(void) {
  ++ticks_ms;
}

bool wio_sdk_init(void) {
  if (!clock_supported()) {
    initialized = false;
    return false;
  }
  if (initialized) {
    const uint32_t systick_enable = SysTick_CTRL_ENABLE_Msk |
                                    SysTick_CTRL_TICKINT_Msk |
                                    SysTick_CTRL_CLKSOURCE_Msk;
    initialized = (SystemCoreClock == WIO_CORE_CLOCK_HZ) && cycle_counter_running() &&
                  ((SysTick->CTRL & systick_enable) == systick_enable) &&
                  (SysTick->LOAD == WIO_CORE_CLOCK_HZ / 1000U - 1U);
    return initialized;
  }
  SystemCoreClockUpdate();
  if (SystemCoreClock != WIO_CORE_CLOCK_HZ) {
    return false;
  }
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  __DSB();
  __ISB();
  if (!cycle_counter_running()) {
    return false;
  }
  ticks_ms = 0U;
  if (SysTick_Config(SystemCoreClock / 1000U) != 0U) {
    return false;
  }
  /* Quiescent on-board outputs only. No external power rails are changed. */
  wio_gpio_output(WIO_PIN_BUZZER, 0U);
  wio_gpio_output(WIO_PIN_IR, 0U);
  initialized = true;
  return true;
}

uint32_t wio_millis(void) {
  return ticks_ms;
}

void wio_delay_us(uint32_t us) {
  /* Bounded chunks avoid multiplication overflow and CYCCNT wrap ambiguity.
   * A finite guard also prevents a disabled DWT from hanging the application.
   */
  while (us != 0U) {
    uint32_t chunk = us > 1000U ? 1000U : us;
    uint32_t cycles = chunk * (SystemCoreClock / 1000000U);
    uint32_t start = DWT->CYCCNT;
    uint32_t guard = cycles + 1000U;
    while ((uint32_t)(DWT->CYCCNT - start) < cycles && guard-- != 0U) {
      __NOP();
    }
    us -= chunk;
  }
}

void wio_delay_ms(uint32_t ms) {
  while (ms-- != 0U) {
    wio_delay_us(1000U);
  }
}

bool wio_gclk_enable(uint32_t channel) {
  if ((channel >= 48U) || !clock_supported()) {
    return false;
  }
  volatile uint32_t *control = &GCLK_REGS->GCLK_PCHCTRL[channel];
  const uint32_t desired = GCLK_PCHCTRL_GEN_GCLK0 | GCLK_PCHCTRL_CHEN_Msk;
  const uint32_t relevant = GCLK_PCHCTRL_GEN_Msk | GCLK_PCHCTRL_CHEN_Msk;
  uint32_t current = *control;

  if ((current & relevant) == desired) {
    return true;  /* Already correct, including a correctly locked channel. */
  }
  if ((current & GCLK_PCHCTRL_WRTLOCK_Msk) != 0U) {
    return false; /* Only a power reset can unlock it; never report success. */
  }
  /* DS60001507N p.145: disable and observe CHEN=0 before changing GEN. */
  if ((current & GCLK_PCHCTRL_CHEN_Msk) != 0U) {
    *control = current & ~GCLK_PCHCTRL_CHEN_Msk;
    uint32_t guard;
    for (guard = WIO_CLOCK_GUARD; guard != 0U; --guard) {
      if ((*control & GCLK_PCHCTRL_CHEN_Msk) == 0U) {
        break;
      }
    }
    if (guard == 0U) {
      return false;
    }
  }
  *control = GCLK_PCHCTRL_GEN_GCLK0;
  if ((*control & relevant) != GCLK_PCHCTRL_GEN_GCLK0) {
    return false;
  }
  *control = desired;
  for (uint32_t guard = WIO_CLOCK_GUARD; guard != 0U; --guard) {
    if ((*control & relevant) == desired) {
      return true;
    }
  }
  return false;
}

bool wio_buzzer_tone(uint32_t frequency_hz, uint32_t duration_ms) {
  if (!initialized || frequency_hz < 40U || frequency_hz > 10000U ||
      duration_ms == 0U || duration_ms > 1000U) {
    return false;
  }
  uint32_t half_us = 500000U / frequency_hz;
  uint32_t periods = (frequency_hz * duration_ms) / 1000U;
  if (periods == 0U) {
    return false;
  }
  for (uint32_t i = 0U; i < periods; ++i) {
    wio_gpio_write(WIO_PIN_BUZZER, 1U);
    wio_delay_us(half_us);
    wio_gpio_write(WIO_PIN_BUZZER, 0U);
    wio_delay_us(half_us);
  }
  return true;
}
