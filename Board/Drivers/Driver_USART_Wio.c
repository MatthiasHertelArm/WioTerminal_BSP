/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Interrupt-driven CMSIS-Driver UARTs for Wio Terminal, 48 MHz SERCOM clock.
 * USART0 is the RTL8720DN eRPC UART, sharing SERCOM0 with SPI0. USART1 is the
 * module's programming/log UART; USART2 is the external header UART.
 * No instance enables the radio, changes its firmware, or drives flow control.
 * Supported: asynchronous 8-N-1, configurable baud, simultaneous Send/Receive.
 * No synchronous, single-wire, IrDA, smart-card, modem, break or RX-timeout support.
 * Buffers must remain valid until completion or an explicit abort.
 * SEND_COMPLETE and TX_COMPLETE are emitted together after the last stop bit.
 * ABORT_SEND releases the application buffer immediately; at most two already
 * queued hardware bytes finish before tx_busy clears, without a completion event.
 * Receive counts include characters flagged with framing/parity errors; inspect
 * status/events before using received data. Receive continues until full or aborted.
 */
#include <stdbool.h>
#include <stddef.h>
#include "Driver_USART.h"
#include "wio_terminal.h"
#include "wio_sdk.h"
#include "wio_sercom.h"

#define USART_CLOCK_HZ        48000000UL
#define USART_SYNC_LIMIT      100000UL
#define USART_TX_IRQS         (SERCOM_USART_INT_INTENSET_DRE_Msk | \
                               SERCOM_USART_INT_INTENSET_TXC_Msk)
#define USART_RX_IRQS         (SERCOM_USART_INT_INTENSET_RXC_Msk | \
                               SERCOM_USART_INT_INTENSET_ERROR_Msk)
#define USART_ALL_IRQS        (USART_TX_IRQS | USART_RX_IRQS)
#define USART_RX_ERRORS       (SERCOM_USART_INT_STATUS_BUFOVF_Msk | \
                               SERCOM_USART_INT_STATUS_FERR_Msk | \
                               SERCOM_USART_INT_STATUS_PERR_Msk)

typedef struct {
  sercom_usart_int_registers_t *regs;
  volatile uint32_t *clock_mask;
  uint32_t clock_bit;
  uint32_t clock_id;
  IRQn_Type irq;
  uint8_t tx_pin, rx_pin, rx_pad;
} USART_PINS;

typedef struct {
  const USART_PINS *pins;
  ARM_USART_SignalEvent_t callback;
  const uint8_t *tx;
  uint8_t *rx;
  uint32_t tx_total, rx_total;
  volatile uint32_t tx_count, rx_count;
  bool initialized;
  bool powered;
  bool configured;
  bool pins_configured;
  bool tx_enabled, rx_enabled;
  bool tx_aborted;
  volatile bool tx_busy, rx_busy;
  volatile bool rx_overflow, rx_framing_error, rx_parity_error;
} USART_STATE;

static const USART_PINS usart0_pins = {
  &SERCOM0_REGS->USART_INT, &MCLK_REGS->MCLK_APBAMASK,
  MCLK_APBAMASK_SERCOM0_Msk, SERCOM0_GCLK_ID_CORE, SERCOM0_0_IRQn,
  WIO_PIN_RTL_ERPC_TX, WIO_PIN_RTL_ERPC_RX, 2U
};
static const USART_PINS usart1_pins = {
  &SERCOM1_REGS->USART_INT, &MCLK_REGS->MCLK_APBAMASK,
  MCLK_APBAMASK_SERCOM1_Msk, SERCOM1_GCLK_ID_CORE, SERCOM1_0_IRQn,
  WIO_PIN_RTL_UART_TX, WIO_PIN_RTL_UART_RX, 1U
};
static const USART_PINS usart2_pins = {
  &SERCOM2_REGS->USART_INT, &MCLK_REGS->MCLK_APBBMASK,
  MCLK_APBBMASK_SERCOM2_Msk, SERCOM2_GCLK_ID_CORE, SERCOM2_0_IRQn,
  WIO_PIN_UART_TX, WIO_PIN_UART_RX, 1U
};
static USART_STATE usart0 = { .pins = &usart0_pins };
static USART_STATE usart1 = { .pins = &usart1_pins };
static USART_STATE usart2 = { .pins = &usart2_pins };
static void usart0_irq(void);

static bool usart_sync (USART_STATE *s) {
  for (uint32_t guard = USART_SYNC_LIMIT; guard != 0U; --guard) {
    if (s->pins->regs->SERCOM_SYNCBUSY == 0U) {
      return true;
    }
  }
  return false;
}

static void usart_nvic (USART_STATE *s, bool enable) {
  for (uint32_t i = 0U; i < 4U; ++i) {
    IRQn_Type irq = (IRQn_Type)((uint32_t)s->pins->irq + i);
    NVIC_DisableIRQ(irq);
    NVIC_ClearPendingIRQ(irq);
    if (enable) {
      NVIC_SetPriority(irq, 3U);
      NVIC_EnableIRQ(irq);
    }
  }
}

static ARM_DRIVER_VERSION usart_version (void) {
  ARM_DRIVER_VERSION version = {
    ARM_USART_API_VERSION, ARM_DRIVER_VERSION_MAJOR_MINOR(1, 0)
  };
  return version;
}

static ARM_USART_CAPABILITIES usart_capabilities (void) {
  ARM_USART_CAPABILITIES capabilities = {0};
  capabilities.asynchronous = 1U;
  capabilities.event_tx_complete = 1U;
  return capabilities;
}

static int32_t usart_initialize (USART_STATE *s, ARM_USART_SignalEvent_t callback) {
  if (s->initialized) {
    return ARM_DRIVER_OK;
  }
  s->callback = callback;
  s->tx_count = 0U;
  s->rx_count = 0U;
  s->initialized = true;
  return ARM_DRIVER_OK;
}

static int32_t usart_power (USART_STATE *s, ARM_POWER_STATE state) {
  sercom_usart_int_registers_t *r = s->pins->regs;
  if (state == ARM_POWER_LOW) {
    return ARM_DRIVER_ERROR_UNSUPPORTED;
  }
  if (state == ARM_POWER_OFF) {
    bool synced = true;
    if (s->powered) {
      usart_nvic(s, false);
      r->SERCOM_INTENCLR = USART_ALL_IRQS;
      r->SERCOM_CTRLA &= ~SERCOM_USART_INT_CTRLA_ENABLE_Msk;
      synced = usart_sync(s);
      if (s->pins_configured) {
        wio_gpio_input(s->pins->tx_pin, 0);
        wio_gpio_input(s->pins->rx_pin, 0);
      }
      *s->pins->clock_mask &= ~s->pins->clock_bit;
    }
    if (s == &usart0) {
      wio_sercom0_release(usart0_irq);
    }
    s->tx_busy = false;
    s->rx_busy = false;
    s->tx_enabled = false;
    s->rx_enabled = false;
    s->configured = false;
    s->pins_configured = false;
    s->powered = false;
    s->tx = NULL;
    s->rx = NULL;
    return synced ? ARM_DRIVER_OK : ARM_DRIVER_ERROR_TIMEOUT;
  }
  if (state != ARM_POWER_FULL) {
    return ARM_DRIVER_ERROR_PARAMETER;
  }
  if (!s->initialized) {
    return ARM_DRIVER_ERROR;
  }
  if (s->powered) {
    return ARM_DRIVER_OK;
  }
  if (s == &usart0 && !wio_sercom0_acquire(usart0_irq)) {
    return ARM_DRIVER_ERROR_BUSY;
  }
  *s->pins->clock_mask |= s->pins->clock_bit;
  if (!wio_gclk_enable(s->pins->clock_id)) {
    *s->pins->clock_mask &= ~s->pins->clock_bit;
    if (s == &usart0) { wio_sercom0_release(usart0_irq); }
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  usart_nvic(s, false);
  r->SERCOM_CTRLA = SERCOM_USART_INT_CTRLA_SWRST_Msk;
  if (!usart_sync(s) || (r->SERCOM_CTRLA & SERCOM_USART_INT_CTRLA_SWRST_Msk) != 0U) {
    *s->pins->clock_mask &= ~s->pins->clock_bit;
    if (s == &usart0) { wio_sercom0_release(usart0_irq); }
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  r->SERCOM_INTENCLR = USART_ALL_IRQS;
  s->configured = false;
  s->tx_enabled = false;
  s->rx_enabled = false;
  s->tx_busy = false;
  s->rx_busy = false;
  s->tx_aborted = false;
  s->tx_count = 0U;
  s->rx_count = 0U;
  s->rx_overflow = false;
  s->rx_framing_error = false;
  s->rx_parity_error = false;
  s->powered = true;
  usart_nvic(s, true);
  return ARM_DRIVER_OK;
}

static int32_t usart_uninitialize (USART_STATE *s) {
  int32_t result = usart_power(s, ARM_POWER_OFF);
  s->initialized = false;
  s->callback = NULL;
  return result;
}

static int32_t usart_baud (uint32_t requested, uint16_t *baud) {
  /* DS60001507N p.853: 16x arithmetic baud generation; round to nearest setting. */
  if (requested == 0U || requested > USART_CLOCK_HZ / 16U) {
    return ARM_USART_ERROR_BAUDRATE;
  }
  uint32_t scale = (uint32_t)(((uint64_t)requested * 16U * 65536U +
                              USART_CLOCK_HZ / 2U) / USART_CLOCK_HZ);
  if (scale == 0U || scale > 65536U) {
    return ARM_USART_ERROR_BAUDRATE;
  }
  uint64_t produced = (uint64_t)USART_CLOCK_HZ * scale;
  uint64_t wanted = (uint64_t)requested * 16U * 65536U;
  uint64_t error = produced > wanted ? produced - wanted : wanted - produced;
  /* Leave at least 1% of the recommended 2% receiver budget for the clock source. */
  if (error * 100U > wanted) {
    return ARM_USART_ERROR_BAUDRATE;
  }
  *baud = (uint16_t)(65536U - scale);
  return ARM_DRIVER_OK;
}

static int32_t usart_abort_send (USART_STATE *s) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  sercom_usart_int_registers_t *r = s->pins->regs;
  r->SERCOM_INTENCLR = SERCOM_USART_INT_INTENCLR_DRE_Msk;
  s->tx = NULL;
  if (s->tx_busy && s->tx_count != 0U) {
    /* Do not reset the shared SERCOM or disrupt a concurrent Receive operation. */
    s->tx_aborted = true;
    r->SERCOM_INTENSET = SERCOM_USART_INT_INTENSET_TXC_Msk;
  } else {
    r->SERCOM_INTENCLR = SERCOM_USART_INT_INTENCLR_TXC_Msk;
    s->tx_busy = false;
    s->tx_aborted = false;
  }
  __DMB();
  __set_PRIMASK(primask);
  return ARM_DRIVER_OK;
}

static int32_t usart_abort_receive (USART_STATE *s) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  sercom_usart_int_registers_t *r = s->pins->regs;
  r->SERCOM_INTENCLR = USART_RX_IRQS;
  s->rx_busy = false;
  s->rx = NULL;
  bool synced = usart_sync(s);
  if (synced) {
    /* Disabling RX flushes the two-byte receive FIFO without touching TX (p.863). */
    r->SERCOM_CTRLB &= ~SERCOM_USART_INT_CTRLB_RXEN_Msk;
    synced = usart_sync(s);
  }
  if (synced && s->rx_enabled) {
    r->SERCOM_CTRLB |= SERCOM_USART_INT_CTRLB_RXEN_Msk;
    synced = usart_sync(s);
    if (synced) {
      r->SERCOM_INTFLAG = SERCOM_USART_INT_INTFLAG_ERROR_Msk;
      r->SERCOM_INTENSET = USART_RX_IRQS;
    }
  }
  if (!synced) {
    s->rx_enabled = false;
    s->configured = false;
  }
  __DMB();
  __set_PRIMASK(primask);
  return synced ? ARM_DRIVER_OK : ARM_DRIVER_ERROR_TIMEOUT;
}

static int32_t usart_enable (USART_STATE *s, bool transmitter, uint32_t arg) {
  if (arg > 1U) {
    return ARM_DRIVER_ERROR_PARAMETER;
  }
  if (!s->configured) {
    return ARM_DRIVER_ERROR;
  }
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  bool current = transmitter ? s->tx_enabled : s->rx_enabled;
  if (current == (arg != 0U)) {
    __set_PRIMASK(primask);
    return ARM_DRIVER_OK;
  }
  if (transmitter ? s->tx_busy : s->rx_busy) {
    __set_PRIMASK(primask);
    return ARM_DRIVER_ERROR_BUSY;
  }
  if (!usart_sync(s)) {
    s->configured = false;
    __set_PRIMASK(primask);
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  sercom_usart_int_registers_t *r = s->pins->regs;
  uint32_t mask = transmitter ? SERCOM_USART_INT_CTRLB_TXEN_Msk :
                               SERCOM_USART_INT_CTRLB_RXEN_Msk;
  if (!transmitter) {
    r->SERCOM_INTENCLR = USART_RX_IRQS;
  }
  if (arg != 0U) {
    r->SERCOM_CTRLB |= mask;
  } else {
    r->SERCOM_CTRLB &= ~mask;
  }
  bool synced = usart_sync(s);
  if (!synced) {
    s->configured = false;
  }
  if (transmitter) {
    s->tx_enabled = synced && arg != 0U;
  } else {
    s->rx_enabled = synced && arg != 0U;
    if (s->rx_enabled) {
      r->SERCOM_INTFLAG = SERCOM_USART_INT_INTFLAG_ERROR_Msk;
      r->SERCOM_INTENSET = USART_RX_IRQS;
    }
  }
  __set_PRIMASK(primask);
  return synced ? ARM_DRIVER_OK : ARM_DRIVER_ERROR_TIMEOUT;
}

static int32_t usart_control (USART_STATE *s, uint32_t control, uint32_t arg) {
  if (!s->powered) {
    return ARM_DRIVER_ERROR;
  }
  uint32_t operation = control & ARM_USART_CONTROL_Msk;
  if (operation != ARM_USART_MODE_ASYNCHRONOUS) {
    if (control != operation) {
      return ARM_DRIVER_ERROR_PARAMETER;
    }
    switch (operation) {
      case ARM_USART_ABORT_SEND:    return usart_abort_send(s);
      case ARM_USART_ABORT_RECEIVE: return usart_abort_receive(s);
      case ARM_USART_CONTROL_TX:    return usart_enable(s, true, arg);
      case ARM_USART_CONTROL_RX:    return usart_enable(s, false, arg);
      default: return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
  }
  if ((control & ARM_USART_DATA_BITS_Msk) != ARM_USART_DATA_BITS_8) {
    return ARM_USART_ERROR_DATA_BITS;
  }
  if ((control & ARM_USART_PARITY_Msk) != ARM_USART_PARITY_NONE) {
    return ARM_USART_ERROR_PARITY;
  }
  if ((control & ARM_USART_STOP_BITS_Msk) != ARM_USART_STOP_BITS_1) {
    return ARM_USART_ERROR_STOP_BITS;
  }
  if ((control & ARM_USART_FLOW_CONTROL_Msk) != ARM_USART_FLOW_CONTROL_NONE) {
    return ARM_USART_ERROR_FLOW_CONTROL;
  }
  if ((control & (ARM_USART_CPOL_Msk | ARM_USART_CPHA_Msk)) != 0U) {
    return ARM_DRIVER_ERROR_UNSUPPORTED;
  }
  uint16_t baud;
  int32_t result = usart_baud(arg, &baud);
  if (result != ARM_DRIVER_OK) {
    return result;
  }
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (s->tx_busy || s->rx_busy) {
    __set_PRIMASK(primask);
    return ARM_DRIVER_ERROR_BUSY;
  }
  sercom_usart_int_registers_t *r = s->pins->regs;
  r->SERCOM_INTENCLR = USART_ALL_IRQS;
  r->SERCOM_CTRLA &= ~SERCOM_USART_INT_CTRLA_ENABLE_Msk;
  bool synced = usart_sync(s);
  if (synced) {
    /* DS60001507N pp.878-879: asynchronous, LSB first, TX pad0;
     * eRPC uses RX pad2, the other UARTs use RX pad1.
     */
    r->SERCOM_CTRLA = SERCOM_USART_INT_CTRLA_MODE_USART_INT_CLK |
                      SERCOM_USART_INT_CTRLA_DORD_Msk |
                      SERCOM_USART_INT_CTRLA_IBON_Msk |
                      SERCOM_USART_INT_CTRLA_RXPO(s->pins->rx_pad) |
                      SERCOM_USART_INT_CTRLA_TXPO(0U);
    r->SERCOM_CTRLB = (s->tx_enabled ? SERCOM_USART_INT_CTRLB_TXEN_Msk : 0U) |
                      (s->rx_enabled ? SERCOM_USART_INT_CTRLB_RXEN_Msk : 0U);
    r->SERCOM_BAUD = baud;
    wio_gpio_output(s->pins->tx_pin, 1U);
    wio_gpio_input(s->pins->rx_pin, 0);
    wio_gpio_function(s->pins->tx_pin, 2U);
    wio_gpio_function(s->pins->rx_pin, 2U);
    s->pins_configured = true;
    r->SERCOM_STATUS = USART_RX_ERRORS;
    r->SERCOM_INTFLAG = SERCOM_USART_INT_INTFLAG_TXC_Msk |
                        SERCOM_USART_INT_INTFLAG_ERROR_Msk;
    r->SERCOM_CTRLA |= SERCOM_USART_INT_CTRLA_ENABLE_Msk;
    synced = usart_sync(s);
  }
  s->configured = synced;
  if (synced && s->rx_enabled) {
    r->SERCOM_INTENSET = USART_RX_IRQS;
  }
  __set_PRIMASK(primask);
  return synced ? ARM_DRIVER_OK : ARM_DRIVER_ERROR_TIMEOUT;
}

static int32_t usart_send (USART_STATE *s, const void *data, uint32_t num) {
  if (data == NULL || num == 0U) {
    return ARM_DRIVER_ERROR_PARAMETER;
  }
  if (!s->powered || !s->configured || !s->tx_enabled) {
    return ARM_DRIVER_ERROR;
  }
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (s->tx_busy) {
    __set_PRIMASK(primask);
    return ARM_DRIVER_ERROR_BUSY;
  }
  sercom_usart_int_registers_t *r = s->pins->regs;
  r->SERCOM_INTENCLR = USART_TX_IRQS;
  r->SERCOM_INTFLAG = SERCOM_USART_INT_INTFLAG_TXC_Msk;
  s->tx = (const uint8_t *)data;
  s->tx_total = num;
  s->tx_count = 0U;
  s->tx_aborted = false;
  s->tx_busy = true;
  __DMB();
  r->SERCOM_INTENSET = SERCOM_USART_INT_INTENSET_DRE_Msk;
  __set_PRIMASK(primask);
  return ARM_DRIVER_OK;
}

static int32_t usart_receive (USART_STATE *s, void *data, uint32_t num) {
  if (data == NULL || num == 0U) {
    return ARM_DRIVER_ERROR_PARAMETER;
  }
  if (!s->powered || !s->configured || !s->rx_enabled) {
    return ARM_DRIVER_ERROR;
  }
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (s->rx_busy) {
    __set_PRIMASK(primask);
    return ARM_DRIVER_ERROR_BUSY;
  }
  s->rx = (uint8_t *)data;
  s->rx_total = num;
  s->rx_count = 0U;
  s->rx_overflow = false;
  s->rx_framing_error = false;
  s->rx_parity_error = false;
  s->rx_busy = true;
  __DMB();
  s->pins->regs->SERCOM_INTENSET = USART_RX_IRQS;
  __set_PRIMASK(primask);
  return ARM_DRIVER_OK;
}

static ARM_USART_STATUS usart_status (USART_STATE *s) {
  ARM_USART_STATUS status = {0};
  status.tx_busy = s->tx_busy;
  status.rx_busy = s->rx_busy;
  status.rx_overflow = s->rx_overflow;
  status.rx_framing_error = s->rx_framing_error;
  status.rx_parity_error = s->rx_parity_error;
  return status;
}

static void usart_interrupt (USART_STATE *s) {
  if (!s->powered) {
    return;
  }
  sercom_usart_int_registers_t *r = s->pins->regs;
  uint32_t flags = r->SERCOM_INTFLAG & r->SERCOM_INTENSET;
  uint32_t events = 0U;
  if ((flags & USART_RX_IRQS) != 0U) {
    /* Read/clear current error status before DATA advances the receive FIFO. */
    uint32_t errors = r->SERCOM_STATUS & USART_RX_ERRORS;
    if ((errors & SERCOM_USART_INT_STATUS_BUFOVF_Msk) != 0U) {
      s->rx_overflow = true;
      events |= ARM_USART_EVENT_RX_OVERFLOW;
    }
    if ((errors & SERCOM_USART_INT_STATUS_FERR_Msk) != 0U) {
      s->rx_framing_error = true;
      events |= ARM_USART_EVENT_RX_FRAMING_ERROR;
    }
    if ((errors & SERCOM_USART_INT_STATUS_PERR_Msk) != 0U) {
      s->rx_parity_error = true;
      events |= ARM_USART_EVENT_RX_PARITY_ERROR;
    }
    r->SERCOM_STATUS = (uint16_t)errors;
    r->SERCOM_INTFLAG = SERCOM_USART_INT_INTFLAG_ERROR_Msk;
    if ((flags & SERCOM_USART_INT_INTFLAG_RXC_Msk) != 0U) {
      uint8_t value = (uint8_t)r->SERCOM_DATA;
      if (s->rx_busy) {
        s->rx[s->rx_count++] = value;
        if (s->rx_count == s->rx_total) {
          __DMB();
          s->rx_busy = false;
          s->rx = NULL;
          events |= ARM_USART_EVENT_RECEIVE_COMPLETE;
        }
      } else {
        /* RX is enabled but the application supplied no buffer: data was lost. */
        if (!s->rx_overflow) {
          events |= ARM_USART_EVENT_RX_OVERFLOW;
        }
        s->rx_overflow = true;
      }
    }
  }
  if ((flags & SERCOM_USART_INT_INTFLAG_TXC_Msk) != 0U) {
    r->SERCOM_INTENCLR = SERCOM_USART_INT_INTENCLR_TXC_Msk;
    r->SERCOM_INTFLAG = SERCOM_USART_INT_INTFLAG_TXC_Msk;
    if (s->tx_busy) {
      bool aborted = s->tx_aborted;
      s->tx_busy = false;
      s->tx_aborted = false;
      s->tx = NULL;
      if (!aborted) {
        events |= ARM_USART_EVENT_SEND_COMPLETE | ARM_USART_EVENT_TX_COMPLETE;
      }
    }
  }
  if ((flags & SERCOM_USART_INT_INTFLAG_DRE_Msk) != 0U) {
    if (s->tx_busy && !s->tx_aborted && s->tx_count < s->tx_total) {
      r->SERCOM_DATA = s->tx[s->tx_count++];
      if (s->tx_count == s->tx_total) {
        r->SERCOM_INTENCLR = SERCOM_USART_INT_INTENCLR_DRE_Msk;
        r->SERCOM_INTENSET = SERCOM_USART_INT_INTENSET_TXC_Msk;
      }
    } else {
      r->SERCOM_INTENCLR = SERCOM_USART_INT_INTENCLR_DRE_Msk;
    }
  }
  /* All state is published before invoking a callback which may start new work. */
  if (events != 0U && s->callback != NULL) {
    s->callback(events);
  }
}

static void usart0_irq(void) { usart_interrupt(&usart0); }

static int32_t usart_transfer (const void *out, void *in, uint32_t num) {
  (void)out; (void)in; (void)num;
  return ARM_DRIVER_ERROR_UNSUPPORTED;
}

static int32_t usart_modem_control (ARM_USART_MODEM_CONTROL control) {
  (void)control;
  return ARM_DRIVER_ERROR_UNSUPPORTED;
}

static ARM_USART_MODEM_STATUS usart_modem_status (void) {
  ARM_USART_MODEM_STATUS status = {0};
  return status;
}

#define USART_INSTANCE(n) \
  static int32_t USART##n##_Initialize(ARM_USART_SignalEvent_t cb) { return usart_initialize(&usart##n, cb); } \
  static int32_t USART##n##_Uninitialize(void) { return usart_uninitialize(&usart##n); } \
  static int32_t USART##n##_PowerControl(ARM_POWER_STATE p) { return usart_power(&usart##n, p); } \
  static int32_t USART##n##_Send(const void *d, uint32_t count) { return usart_send(&usart##n, d, count); } \
  static int32_t USART##n##_Receive(void *d, uint32_t count) { return usart_receive(&usart##n, d, count); } \
  static uint32_t USART##n##_GetTxCount(void) { return usart##n.tx_count; } \
  static uint32_t USART##n##_GetRxCount(void) { return usart##n.rx_count; } \
  static int32_t USART##n##_Control(uint32_t c, uint32_t a) { return usart_control(&usart##n, c, a); } \
  static ARM_USART_STATUS USART##n##_GetStatus(void) { return usart_status(&usart##n); } \
  ARM_DRIVER_USART Driver_USART##n = { usart_version, usart_capabilities, \
    USART##n##_Initialize, USART##n##_Uninitialize, USART##n##_PowerControl, \
    USART##n##_Send, USART##n##_Receive, usart_transfer, \
    USART##n##_GetTxCount, USART##n##_GetRxCount, USART##n##_Control, USART##n##_GetStatus, \
    usart_modem_control, usart_modem_status };

#define USART_IRQS(n) \
  void SERCOM##n##_0_Handler(void) { usart_interrupt(&usart##n); } \
  void SERCOM##n##_1_Handler(void) { usart_interrupt(&usart##n); } \
  void SERCOM##n##_2_Handler(void) { usart_interrupt(&usart##n); } \
  void SERCOM##n##_OTHER_Handler(void) { usart_interrupt(&usart##n); }

USART_INSTANCE(0)
USART_INSTANCE(1)
USART_INSTANCE(2)
USART_IRQS(1)
USART_IRQS(2)
