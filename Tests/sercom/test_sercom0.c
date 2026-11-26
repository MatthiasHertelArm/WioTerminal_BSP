/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "wio_sercom.h"
#include "wio_sdk.h"

void SERCOM0_0_Handler(void);
void SERCOM0_1_Handler(void);
void SERCOM0_2_Handler(void);
void SERCOM0_OTHER_Handler(void);

static uint32_t primask, disabled, cleared, a_count, b_count;
uint32_t __get_PRIMASK(void) { return primask; }
void __disable_irq(void) { primask = 1U; }
void __set_PRIMASK(uint32_t value) { primask = value; }
void __DMB(void) { }
void NVIC_DisableIRQ(IRQn_Type irq) {
  assert(primask == 1U);
  assert(irq >= SERCOM0_0_IRQn && irq < SERCOM0_0_IRQn + 4);
  disabled |= 1U << (uint32_t)(irq - SERCOM0_0_IRQn);
}
void NVIC_ClearPendingIRQ(IRQn_Type irq) {
  assert(primask == 1U);
  assert(irq >= SERCOM0_0_IRQn && irq < SERCOM0_0_IRQn + 4);
  cleared |= 1U << (uint32_t)(irq - SERCOM0_0_IRQn);
}
static void a(void) { ++a_count; }
static void b(void) { ++b_count; }

int main(void) {
  assert(!wio_sercom0_acquire(NULL));
  assert(primask == 0U && disabled == 0U && cleared == 0U);
  SERCOM0_0_Handler();
  assert(a_count == 0U && b_count == 0U);

  assert(wio_sercom0_acquire(a));
  assert(primask == 0U && disabled == 15U && cleared == 15U);
  disabled = cleared = 0U;
  assert(wio_sercom0_acquire(a));
  assert(!wio_sercom0_acquire(b));
  assert(disabled == 0U && cleared == 0U);
  wio_sercom0_release(b);
  wio_sercom0_release(NULL);
  assert(!wio_sercom0_acquire(b));
  SERCOM0_0_Handler();
  SERCOM0_1_Handler();
  SERCOM0_2_Handler();
  SERCOM0_OTHER_Handler();
  assert(a_count == 4U && b_count == 0U);

  wio_sercom0_release(a);
  SERCOM0_0_Handler();
  assert(a_count == 4U);
  primask = 1U;
  assert(wio_sercom0_acquire(b));
  assert(primask == 1U && disabled == 15U && cleared == 15U);
  SERCOM0_OTHER_Handler();
  assert(b_count == 1U);
  wio_sercom0_release(b);
  assert(primask == 1U);
  puts("SERCOM0 ownership, dispatch, stale IRQ and PRIMASK tests passed");
  return 0;
}
