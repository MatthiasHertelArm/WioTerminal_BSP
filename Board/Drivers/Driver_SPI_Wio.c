/* SPDX-License-Identifier: Apache-2.0 */
/* Interrupt-driven CMSIS-Driver SPI masters for the ATSAMD51 Wio Terminal. */
#include <stdbool.h>
#include <stddef.h>
#include "Driver_SPI.h"
#include "wio_terminal.h"
#include "wio_sdk.h"
#include "wio_sercom.h"

#define SPI_CLOCK_HZ        48000000UL
#define SPI_SYNC_LIMIT      100000UL
#define SPI_IRQ_FLAGS       (SERCOM_SPIM_INTENSET_DRE_Msk | \
                             SERCOM_SPIM_INTENSET_TXC_Msk | \
                             SERCOM_SPIM_INTENSET_RXC_Msk | \
                             SERCOM_SPIM_INTENSET_ERROR_Msk)

typedef struct {
  sercom_spim_registers_t *regs;
  volatile uint32_t *clock_mask;
  uint32_t clock_bit;
  uint32_t clock_id;
  IRQn_Type irq;
  uint8_t mosi, miso, sck, ss, mux, dopo;
} SPI_PINS;

typedef struct {
  const SPI_PINS *pins;
  ARM_SPI_SignalEvent_t callback;
  const uint8_t *tx;
  uint8_t *rx;
  uint32_t total;
  uint32_t sent;
  volatile uint32_t received;
  uint32_t speed;
  uint8_t default_tx;
  bool initialized;
  bool powered;
  volatile bool configured;
  bool software_ss;
  volatile bool busy;
  volatile bool data_lost;
  bool tx_complete;
} SPI_STATE;

static const SPI_PINS spi0_pins = {
  &SERCOM0_REGS->SPIM, &MCLK_REGS->MCLK_APBAMASK,
  MCLK_APBAMASK_SERCOM0_Msk, SERCOM0_GCLK_ID_CORE, SERCOM0_0_IRQn,
  WIO_PIN_RTL_SPI_MOSI, WIO_PIN_RTL_SPI_MISO, WIO_PIN_RTL_SPI_SCK,
  WIO_PIN_RTL_SPI_CS, 2U, 0U
};
static const SPI_PINS spi5_pins = {
  &SERCOM5_REGS->SPIM, &MCLK_REGS->MCLK_APBDMASK,
  MCLK_APBDMASK_SERCOM5_Msk, SERCOM5_GCLK_ID_CORE, SERCOM5_0_IRQn,
  WIO_PIN_SPI_MOSI, WIO_PIN_SPI_MISO, WIO_PIN_SPI_SCK,
  WIO_PIN_SPI_CS, 3U, 0U
};
static const SPI_PINS spi6_pins = {
  &SERCOM6_REGS->SPIM, &MCLK_REGS->MCLK_APBDMASK,
  MCLK_APBDMASK_SERCOM6_Msk, SERCOM6_GCLK_ID_CORE, SERCOM6_0_IRQn,
  WIO_PIN_SD_MOSI, WIO_PIN_SD_MISO, WIO_PIN_SD_SCK,
  WIO_PIN_SD_CS, 2U, 0U
};
static const SPI_PINS spi7_pins = {
  &SERCOM7_REGS->SPIM, &MCLK_REGS->MCLK_APBDMASK,
  MCLK_APBDMASK_SERCOM7_Msk, SERCOM7_GCLK_ID_CORE, SERCOM7_0_IRQn,
  WIO_PIN_LCD_MOSI, WIO_PIN_LCD_MISO, WIO_PIN_LCD_SCK,
  WIO_PIN_LCD_CS, 3U, 2U
};
static SPI_STATE spi0 = { .pins = &spi0_pins };
static SPI_STATE spi5 = { .pins = &spi5_pins };
static SPI_STATE spi6 = { .pins = &spi6_pins };
static SPI_STATE spi7 = { .pins = &spi7_pins };
static void spi0_irq(void);

static bool spi_sync (SPI_STATE *s) {
  for (uint32_t guard = SPI_SYNC_LIMIT; guard != 0U; --guard) {
    if (s->pins->regs->SERCOM_SYNCBUSY == 0U) {
      return true;
    }
  }
  return false;
}

static void spi_nvic (SPI_STATE *s, bool enable) {
  /* Each SERCOM has four consecutive NVIC lines: DRE, TXC, RXC, OTHER. */
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

static void spi_drain (SPI_STATE *s) {
  /* The receiver is double-buffered, never an unbounded drain loop. */
  for (uint32_t i = 0U; i < 2U; ++i) {
    if ((s->pins->regs->SERCOM_INTFLAG & SERCOM_SPIM_INTFLAG_RXC_Msk) != 0U) {
      (void)s->pins->regs->SERCOM_DATA;
    }
  }
}

static ARM_DRIVER_VERSION spi_version (void) {
  ARM_DRIVER_VERSION version = { ARM_SPI_API_VERSION, ARM_DRIVER_VERSION_MAJOR_MINOR(1, 0) };
  return version;
}

static ARM_SPI_CAPABILITIES spi_capabilities (void) {
  ARM_SPI_CAPABILITIES capabilities = {0};
  /* No simplex/slave, TI SSI, Microwire, or mode-fault detection. */
  return capabilities;
}

static int32_t spi_initialize (SPI_STATE *s, ARM_SPI_SignalEvent_t callback) {
  if (s->initialized) {
    return ARM_DRIVER_OK;
  }
  s->callback = callback;
  s->default_tx = 0xFFU;
  s->received = 0U;
  s->data_lost = false;
  s->initialized = true;
  return ARM_DRIVER_OK;
}

static int32_t spi_power (SPI_STATE *s, ARM_POWER_STATE state) {
  sercom_spim_registers_t *r = s->pins->regs;
  if (state == ARM_POWER_LOW) {
    return ARM_DRIVER_ERROR_UNSUPPORTED;
  }
  if (state == ARM_POWER_OFF) {
    bool synced = true;
    if (s->powered) {
      spi_nvic(s, false);
      r->SERCOM_INTENCLR = SPI_IRQ_FLAGS;
      r->SERCOM_CTRLA &= ~SERCOM_SPIM_CTRLA_ENABLE_Msk;
      synced = spi_sync(s);
      wio_gpio_input(s->pins->mosi, 0);
      wio_gpio_input(s->pins->miso, 0);
      wio_gpio_input(s->pins->sck, 0);
      if (s->software_ss) {
        wio_gpio_write(s->pins->ss, 1U);
        wio_gpio_input(s->pins->ss, 1);
      }
      *s->pins->clock_mask &= ~s->pins->clock_bit;
    }
    if (s == &spi0) {
      wio_sercom0_release(spi0_irq);
    }
    s->busy = false;
    s->configured = false;
    s->software_ss = false;
    s->powered = false;
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
  if (s == &spi0 && !wio_sercom0_acquire(spi0_irq)) {
    return ARM_DRIVER_ERROR_BUSY;
  }
  *s->pins->clock_mask |= s->pins->clock_bit;
  if (!wio_gclk_enable(s->pins->clock_id)) {
    *s->pins->clock_mask &= ~s->pins->clock_bit;
    if (s == &spi0) { wio_sercom0_release(spi0_irq); }
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  spi_nvic(s, false);
  r->SERCOM_CTRLA = SERCOM_SPIM_CTRLA_SWRST_Msk;
  if (!spi_sync(s) || (r->SERCOM_CTRLA & SERCOM_SPIM_CTRLA_SWRST_Msk) != 0U) {
    *s->pins->clock_mask &= ~s->pins->clock_bit;
    if (s == &spi0) { wio_sercom0_release(spi0_irq); }
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  r->SERCOM_INTENCLR = SPI_IRQ_FLAGS;
  /* Control(MODE_MASTER, ...) enables the peripheral and assigns its pins. */
  s->powered = true;
  s->configured = false;
  s->busy = false;
  s->data_lost = false;
  s->received = 0U;
  spi_nvic(s, true);
  return ARM_DRIVER_OK;
}

static int32_t spi_uninitialize (SPI_STATE *s) {
  int32_t result = spi_power(s, ARM_POWER_OFF);
  s->initialized = false;
  s->callback = NULL;
  return result;
}

static int32_t spi_abort (SPI_STATE *s) {
  sercom_spim_registers_t *r = s->pins->regs;
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  r->SERCOM_INTENCLR = SPI_IRQ_FLAGS;
  r->SERCOM_CTRLA &= ~SERCOM_SPIM_CTRLA_ENABLE_Msk;
  bool synced = spi_sync(s);
  if (synced) {
    spi_drain(s);
    r->SERCOM_STATUS = SERCOM_SPIM_STATUS_BUFOVF_Msk;
    r->SERCOM_INTFLAG = SERCOM_SPIM_INTFLAG_TXC_Msk | SERCOM_SPIM_INTFLAG_ERROR_Msk;
    if (s->configured) {
      r->SERCOM_CTRLA |= SERCOM_SPIM_CTRLA_ENABLE_Msk;
      synced = spi_sync(s);
    }
  }
  if (!synced) {
    s->configured = false;
  }
  s->busy = false;
  s->tx_complete = false;
  __DMB();
  __set_PRIMASK(primask);
  return synced ? ARM_DRIVER_OK : ARM_DRIVER_ERROR_TIMEOUT;
}

static int32_t spi_baud (uint32_t speed, uint8_t *baud) {
  if (speed == 0U) {
    return ARM_DRIVER_ERROR_PARAMETER;
  }
  uint32_t divider = (speed >= SPI_CLOCK_HZ / 2U) ? 1U :
                    (SPI_CLOCK_HZ / 2U + speed - 1U) / speed;
  if (divider > 256U) {
    return ARM_DRIVER_ERROR_PARAMETER;
  }
  *baud = (uint8_t)(divider - 1U);
  return ARM_DRIVER_OK;
}

static int32_t spi_control (SPI_STATE *s, uint32_t control, uint32_t arg) {
  sercom_spim_registers_t *r = s->pins->regs;
  uint32_t operation = control & ARM_SPI_CONTROL_Msk;
  if (!s->powered) {
    return ARM_DRIVER_ERROR;
  }
  if (operation == ARM_SPI_ABORT_TRANSFER) {
    return spi_abort(s);
  }
  if (operation == ARM_SPI_GET_BUS_SPEED) {
    return s->configured ? (int32_t)s->speed : ARM_DRIVER_ERROR;
  }
  if (s->busy) {
    return ARM_DRIVER_ERROR_BUSY;
  }
  if (operation == ARM_SPI_SET_DEFAULT_TX_VALUE) {
    if (arg > 0xFFU) {
      return ARM_DRIVER_ERROR_PARAMETER;
    }
    s->default_tx = (uint8_t)arg;
    return ARM_DRIVER_OK;
  }
  if (operation == ARM_SPI_CONTROL_SS) {
    if (!s->configured || !s->software_ss) {
      return ARM_DRIVER_ERROR_UNSUPPORTED;
    }
    if (arg != ARM_SPI_SS_ACTIVE && arg != ARM_SPI_SS_INACTIVE) {
      return ARM_DRIVER_ERROR_PARAMETER;
    }
    wio_gpio_write(s->pins->ss, arg == ARM_SPI_SS_ACTIVE ? 0U : 1U);
    return ARM_DRIVER_OK;
  }
  if (operation == ARM_SPI_MODE_INACTIVE) {
    if (s->software_ss) {
      wio_gpio_write(s->pins->ss, 1U);
    }
    r->SERCOM_CTRLA &= ~SERCOM_SPIM_CTRLA_ENABLE_Msk;
    s->configured = false;
    return spi_sync(s) ? ARM_DRIVER_OK : ARM_DRIVER_ERROR_TIMEOUT;
  }
  uint8_t baud;
  if (operation == ARM_SPI_SET_BUS_SPEED) {
    if (!s->configured) {
      return ARM_DRIVER_ERROR;
    }
    int32_t result = spi_baud(arg, &baud);
    if (result != ARM_DRIVER_OK) {
      return result;
    }
    r->SERCOM_CTRLA &= ~SERCOM_SPIM_CTRLA_ENABLE_Msk;
    if (!spi_sync(s)) {
      s->configured = false;
      return ARM_DRIVER_ERROR_TIMEOUT;
    }
    r->SERCOM_BAUD = baud;
    r->SERCOM_CTRLA |= SERCOM_SPIM_CTRLA_ENABLE_Msk;
    if (!spi_sync(s)) {
      s->configured = false;
      return ARM_DRIVER_ERROR_TIMEOUT;
    }
    s->speed = SPI_CLOCK_HZ / (2U * ((uint32_t)baud + 1U));
    return ARM_DRIVER_OK;
  }
  if (operation != ARM_SPI_MODE_MASTER) {
    return ARM_SPI_ERROR_MODE;
  }
  if ((control & ARM_SPI_DATA_BITS_Msk) != ARM_SPI_DATA_BITS(8U)) {
    return ARM_SPI_ERROR_DATA_BITS;
  }
  uint32_t format = control & ARM_SPI_FRAME_FORMAT_Msk;
  if (format != ARM_SPI_CPOL0_CPHA0 && format != ARM_SPI_CPOL0_CPHA1 &&
      format != ARM_SPI_CPOL1_CPHA0 && format != ARM_SPI_CPOL1_CPHA1) {
    return ARM_SPI_ERROR_FRAME_FORMAT;
  }
  uint32_t ss_mode = control & ARM_SPI_SS_MASTER_MODE_Msk;
  if (ss_mode != ARM_SPI_SS_MASTER_UNUSED && ss_mode != ARM_SPI_SS_MASTER_SW) {
    return ARM_SPI_ERROR_SS_MODE;
  }
  if ((control & ARM_SPI_SS_SLAVE_MODE_Msk) != 0U) {
    return ARM_SPI_ERROR_SS_MODE;
  }
  int32_t result = spi_baud(arg, &baud);
  if (result != ARM_DRIVER_OK) {
    return result;
  }
  /* Put software SS inactive before pinmux or clock-polarity changes. */
  if (s->software_ss || ss_mode == ARM_SPI_SS_MASTER_SW) {
    wio_gpio_output(s->pins->ss, 1U);
  }
  r->SERCOM_INTENCLR = SPI_IRQ_FLAGS;
  r->SERCOM_CTRLA &= ~SERCOM_SPIM_CTRLA_ENABLE_Msk;
  if (!spi_sync(s)) {
    s->configured = false;
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  uint32_t ctrla = SERCOM_SPIM_CTRLA_MODE_SPI_MASTER |
                   SERCOM_SPIM_CTRLA_IBON_Msk |
                   SERCOM_SPIM_CTRLA_DIPO(2U) |
                   SERCOM_SPIM_CTRLA_DOPO(s->pins->dopo);
  if (format == ARM_SPI_CPOL1_CPHA0 || format == ARM_SPI_CPOL1_CPHA1) {
    ctrla |= SERCOM_SPIM_CTRLA_CPOL_Msk;
  }
  if (format == ARM_SPI_CPOL0_CPHA1 || format == ARM_SPI_CPOL1_CPHA1) {
    ctrla |= SERCOM_SPIM_CTRLA_CPHA_Msk;
  }
  if ((control & ARM_SPI_BIT_ORDER_Msk) == ARM_SPI_LSB_MSB) {
    ctrla |= SERCOM_SPIM_CTRLA_DORD_Msk;
  }
  r->SERCOM_CTRLA = ctrla;
  r->SERCOM_CTRLB = SERCOM_SPIM_CTRLB_RXEN_Msk;
  r->SERCOM_BAUD = baud;
  if (!spi_sync(s)) {
    s->configured = false;
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  wio_gpio_input(s->pins->miso, 0);
  wio_gpio_output(s->pins->mosi, 0U);
  wio_gpio_output(s->pins->sck, (ctrla & SERCOM_SPIM_CTRLA_CPOL_Msk) != 0U);
  wio_gpio_function(s->pins->mosi, s->pins->mux);
  wio_gpio_function(s->pins->miso, s->pins->mux);
  wio_gpio_function(s->pins->sck, s->pins->mux);
  if (ss_mode == ARM_SPI_SS_MASTER_UNUSED && s->software_ss) {
    wio_gpio_input(s->pins->ss, 0);
  }
  s->software_ss = (ss_mode == ARM_SPI_SS_MASTER_SW);
  spi_drain(s);
  r->SERCOM_STATUS = SERCOM_SPIM_STATUS_BUFOVF_Msk;
  r->SERCOM_INTFLAG = SERCOM_SPIM_INTFLAG_TXC_Msk | SERCOM_SPIM_INTFLAG_ERROR_Msk;
  r->SERCOM_CTRLA |= SERCOM_SPIM_CTRLA_ENABLE_Msk;
  if (!spi_sync(s)) {
    s->configured = false;
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  s->speed = SPI_CLOCK_HZ / (2U * ((uint32_t)baud + 1U));
  s->configured = true;
  return ARM_DRIVER_OK;
}

static int32_t spi_transfer (SPI_STATE *s, const void *tx, void *rx, uint32_t num) {
  if (num == 0U || (tx == NULL && rx == NULL)) {
    return ARM_DRIVER_ERROR_PARAMETER;
  }
  if (!s->powered || !s->configured) {
    return ARM_DRIVER_ERROR;
  }
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (s->busy) {
    __set_PRIMASK(primask);
    return ARM_DRIVER_ERROR_BUSY;
  }
  sercom_spim_registers_t *r = s->pins->regs;
  r->SERCOM_INTENCLR = SPI_IRQ_FLAGS;
  spi_drain(s);
  r->SERCOM_STATUS = SERCOM_SPIM_STATUS_BUFOVF_Msk;
  r->SERCOM_INTFLAG = SERCOM_SPIM_INTFLAG_TXC_Msk | SERCOM_SPIM_INTFLAG_ERROR_Msk;
  s->tx = (const uint8_t *)tx;
  s->rx = (uint8_t *)rx;
  s->total = num;
  s->sent = 0U;
  s->received = 0U;
  s->tx_complete = false;
  s->data_lost = false;
  s->busy = true;
  __DMB();
  r->SERCOM_INTENSET = SERCOM_SPIM_INTENSET_RXC_Msk |
                       SERCOM_SPIM_INTENSET_ERROR_Msk |
                       SERCOM_SPIM_INTENSET_DRE_Msk;
  __set_PRIMASK(primask);
  return ARM_DRIVER_OK;
}

static ARM_SPI_STATUS spi_status (SPI_STATE *s) {
  ARM_SPI_STATUS status = {0};
  status.busy = s->busy;
  status.data_lost = s->data_lost;
  return status;
}

static void spi_interrupt (SPI_STATE *s) {
  sercom_spim_registers_t *r = s->pins->regs;
  if (!s->powered) {
    return;
  }
  uint32_t flags = r->SERCOM_INTFLAG & r->SERCOM_INTENSET;
  if (!s->busy) {
    r->SERCOM_INTENCLR = SPI_IRQ_FLAGS;
    return;
  }
  if ((flags & SERCOM_SPIM_INTFLAG_ERROR_Msk) != 0U ||
      (r->SERCOM_STATUS & SERCOM_SPIM_STATUS_BUFOVF_Msk) != 0U) {
    s->data_lost = true;
    (void)spi_abort(s);
    if (s->callback != NULL) {
      s->callback(ARM_SPI_EVENT_DATA_LOST);
    }
    return;
  }
  if ((flags & SERCOM_SPIM_INTFLAG_RXC_Msk) != 0U) {
    uint8_t value = (uint8_t)r->SERCOM_DATA;
    if (s->received < s->total) {
      if (s->rx != NULL) {
        s->rx[s->received] = value;
      }
      ++s->received;
    }
    if (s->sent < s->total) {
      r->SERCOM_INTENSET = SERCOM_SPIM_INTENSET_DRE_Msk;
    }
  }
  if ((flags & SERCOM_SPIM_INTFLAG_TXC_Msk) != 0U) {
    r->SERCOM_INTENCLR = SERCOM_SPIM_INTENCLR_TXC_Msk;
    r->SERCOM_INTFLAG = SERCOM_SPIM_INTFLAG_TXC_Msk;
    s->tx_complete = true;
  }
  if (s->tx_complete && s->received == s->total) {
    r->SERCOM_INTENCLR = SPI_IRQ_FLAGS;
    __DMB();
    s->busy = false;
    if (s->callback != NULL) {
      s->callback(ARM_SPI_EVENT_TRANSFER_COMPLETE);
    }
    return;
  }
  if ((r->SERCOM_INTFLAG & r->SERCOM_INTENSET & SERCOM_SPIM_INTFLAG_DRE_Msk) != 0U) {
    r->SERCOM_INTENCLR = SERCOM_SPIM_INTENCLR_DRE_Msk;
    /* Only one byte is outstanding, so slow interrupt service cannot overrun RX. */
    if (s->sent < s->total && s->sent == s->received) {
      uint8_t value = (s->tx != NULL) ? s->tx[s->sent] : s->default_tx;
      ++s->sent;
      r->SERCOM_INTFLAG = SERCOM_SPIM_INTFLAG_TXC_Msk;
      r->SERCOM_DATA = value;
      if (s->sent == s->total) {
        r->SERCOM_INTENSET = SERCOM_SPIM_INTENSET_TXC_Msk;
      }
    }
  }
}

static void spi0_irq(void) { spi_interrupt(&spi0); }

#define SPI_INSTANCE(n) \
  static int32_t SPI##n##_Initialize(ARM_SPI_SignalEvent_t cb) { return spi_initialize(&spi##n, cb); } \
  static int32_t SPI##n##_Uninitialize(void) { return spi_uninitialize(&spi##n); } \
  static int32_t SPI##n##_PowerControl(ARM_POWER_STATE p) { return spi_power(&spi##n, p); } \
  static int32_t SPI##n##_Send(const void *d, uint32_t count) { \
    return d != NULL ? spi_transfer(&spi##n, d, NULL, count) : ARM_DRIVER_ERROR_PARAMETER; \
  } \
  static int32_t SPI##n##_Receive(void *d, uint32_t count) { \
    return d != NULL ? spi_transfer(&spi##n, NULL, d, count) : ARM_DRIVER_ERROR_PARAMETER; \
  } \
  static int32_t SPI##n##_Transfer(const void *out, void *in, uint32_t count) { \
    return out != NULL && in != NULL ? spi_transfer(&spi##n, out, in, count) : ARM_DRIVER_ERROR_PARAMETER; \
  } \
  static uint32_t SPI##n##_GetDataCount(void) { return spi##n.received; } \
  static int32_t SPI##n##_Control(uint32_t c, uint32_t a) { return spi_control(&spi##n, c, a); } \
  static ARM_SPI_STATUS SPI##n##_GetStatus(void) { return spi_status(&spi##n); } \
  ARM_DRIVER_SPI Driver_SPI##n = { spi_version, spi_capabilities, \
    SPI##n##_Initialize, SPI##n##_Uninitialize, SPI##n##_PowerControl, \
    SPI##n##_Send, SPI##n##_Receive, SPI##n##_Transfer, SPI##n##_GetDataCount, \
    SPI##n##_Control, SPI##n##_GetStatus };

#define SPI_IRQS(n) \
  void SERCOM##n##_0_Handler(void) { spi_interrupt(&spi##n); } \
  void SERCOM##n##_1_Handler(void) { spi_interrupt(&spi##n); } \
  void SERCOM##n##_2_Handler(void) { spi_interrupt(&spi##n); } \
  void SERCOM##n##_OTHER_Handler(void) { spi_interrupt(&spi##n); }

SPI_INSTANCE(0)
SPI_INSTANCE(5)
SPI_INSTANCE(6)
SPI_INSTANCE(7)
SPI_IRQS(5)
SPI_IRQS(6)
SPI_IRQS(7)
