/* SPDX-License-Identifier: Apache-2.0 */
#include <stddef.h>
#include "wio_sercom.h"
#include "wio_sdk.h"

static wio_sercom_irq_t volatile owner;

bool wio_sercom0_acquire(wio_sercom_irq_t handler) {
  if (handler == NULL) {
    return false;
  }
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  bool available = owner == NULL || owner == handler;
  if (available) {
    if (owner == NULL) {
      /* Quiesce any interrupt inherited from a previous application before
       * publishing the new, not-yet-powered driver's handler. */
      for (uint32_t i = 0U; i < 4U; ++i) {
        IRQn_Type irq = (IRQn_Type)((int32_t)SERCOM0_0_IRQn + (int32_t)i);
        NVIC_DisableIRQ(irq);
        NVIC_ClearPendingIRQ(irq);
      }
    }
    owner = handler;
    __DMB();
  }
  __set_PRIMASK(primask);
  return available;
}

void wio_sercom0_release(wio_sercom_irq_t handler) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (owner == handler) {
    owner = NULL;
    __DMB();
  }
  __set_PRIMASK(primask);
}

static void dispatch(void) {
  wio_sercom_irq_t handler = owner;
  if (handler != NULL) {
    handler();
  }
}

void SERCOM0_0_Handler(void) { dispatch(); }
void SERCOM0_1_Handler(void) { dispatch(); }
void SERCOM0_2_Handler(void) { dispatch(); }
void SERCOM0_OTHER_Handler(void) { dispatch(); }
