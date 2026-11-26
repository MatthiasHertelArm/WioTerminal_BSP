/* SPDX-License-Identifier: Apache-2.0 */
/* Interrupt-driven CMSIS-Driver I2C masters for the ATSAMD51 Wio Terminal. */
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include "Driver_I2C.h"
#include "wio_terminal.h"
#include "wio_sdk.h"

#define I2C_CLOCK_HZ       48000000UL
#define I2C_SYNC_LIMIT     100000UL
#define I2C_BUS_IDLE       1U
#define I2C_BUS_OWNER      2U
#define I2C_COMMAND_READ   2U
#define I2C_COMMAND_STOP   3U
#define I2C_IRQ_FLAGS      (SERCOM_I2CM_INTENSET_MB_Msk | \
                            SERCOM_I2CM_INTENSET_SB_Msk | \
                            SERCOM_I2CM_INTENSET_ERROR_Msk)
#define I2C_ERROR_FLAGS    (SERCOM_I2CM_STATUS_BUSERR_Msk | \
                            SERCOM_I2CM_STATUS_ARBLOST_Msk)

typedef struct {
  sercom_i2cm_registers_t *regs;
  volatile uint32_t *clock_mask;
  uint32_t clock_bit;
  uint32_t clock_id;
  IRQn_Type irq;
  uint8_t sda, scl;
} I2C_PINS;

typedef struct {
  const I2C_PINS *pins;
  ARM_I2C_SignalEvent_t callback;
  const uint8_t *tx;
  uint8_t *rx;
  uint32_t total;
  uint32_t sent;
  volatile uint32_t count;
  uint8_t baud;
  uint8_t baud_low;
  bool initialized;
  bool powered;
  volatile bool faulted;
  bool receive;
  bool pending;
  volatile bool held;
  volatile bool busy;
  volatile bool arbitration_lost;
  volatile bool bus_error;
} I2C_STATE;

static const I2C_PINS i2c3_pins = {
  &SERCOM3_REGS->I2CM, &MCLK_REGS->MCLK_APBBMASK,
  MCLK_APBBMASK_SERCOM3_Msk, SERCOM3_GCLK_ID_CORE, SERCOM3_0_IRQn,
  WIO_PIN_I2C_SDA, WIO_PIN_I2C_SCL
};
static const I2C_PINS i2c4_pins = {
  &SERCOM4_REGS->I2CM, &MCLK_REGS->MCLK_APBDMASK,
  MCLK_APBDMASK_SERCOM4_Msk, SERCOM4_GCLK_ID_CORE, SERCOM4_0_IRQn,
  WIO_PIN_ACC_SDA, WIO_PIN_ACC_SCL
};
static I2C_STATE i2c3 = { .pins = &i2c3_pins };
static I2C_STATE i2c4 = { .pins = &i2c4_pins };

static bool i2c_sync (I2C_STATE *s) {
  for (uint32_t guard = I2C_SYNC_LIMIT; guard != 0U; --guard) {
    if (s->pins->regs->SERCOM_SYNCBUSY == 0U) {
      return true;
    }
  }
  return false;
}

static uint32_t i2c_bus_state (I2C_STATE *s) {
  return (s->pins->regs->SERCOM_STATUS & SERCOM_I2CM_STATUS_BUSSTATE_Msk) >>
         SERCOM_I2CM_STATUS_BUSSTATE_Pos;
}

static void i2c_clear_errors (I2C_STATE *s) {
  sercom_i2cm_registers_t *r = s->pins->regs;
  /* BUSSTATE=0 has no effect; do not write IDLE while clearing W1C errors. */
  r->SERCOM_STATUS = I2C_ERROR_FLAGS;
  r->SERCOM_INTFLAG = SERCOM_I2CM_INTFLAG_ERROR_Msk;
}

static uint32_t i2c_latch_errors (I2C_STATE *s) {
  uint32_t status = s->pins->regs->SERCOM_STATUS;
  uint32_t events = 0U;
  /* A bus error while OWNER sets both BUSERR and ARBLOST (DS60001507N p.990). */
  if ((status & SERCOM_I2CM_STATUS_ARBLOST_Msk) != 0U) {
    s->arbitration_lost = true;
    events |= ARM_I2C_EVENT_ARBITRATION_LOST;
  }
  if ((status & SERCOM_I2CM_STATUS_BUSERR_Msk) != 0U ||
      (events == 0U &&
       (s->pins->regs->SERCOM_INTFLAG & SERCOM_I2CM_INTFLAG_ERROR_Msk) != 0U)) {
    s->bus_error = true;
    events |= ARM_I2C_EVENT_BUS_ERROR;
  }
  return events;
}

static void i2c_nvic (I2C_STATE *s, bool enable) {
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

static bool i2c_command (I2C_STATE *s, uint32_t command, bool nack) {
  sercom_i2cm_registers_t *r = s->pins->regs;
  if (!i2c_sync(s)) {
    return false;
  }
  r->SERCOM_CTRLB = (r->SERCOM_CTRLB &
                    ~(SERCOM_I2CM_CTRLB_CMD_Msk | SERCOM_I2CM_CTRLB_ACKACT_Msk)) |
                    SERCOM_I2CM_CTRLB_CMD(command) |
                    (nack ? SERCOM_I2CM_CTRLB_ACKACT_Msk : 0U);
  return i2c_sync(s);
}

static bool i2c_stop (I2C_STATE *s, bool nack) {
  sercom_i2cm_registers_t *r = s->pins->regs;
  if (!i2c_sync(s)) {
    return false;
  }
  /* CMD is legal only with MB/SB set. An abort may arrive halfway through a byte. */
  uint32_t guard;
  for (guard = I2C_SYNC_LIMIT; guard != 0U; --guard) {
    if (i2c_bus_state(s) != I2C_BUS_OWNER) {
      return true;
    }
    if ((r->SERCOM_INTFLAG & (SERCOM_I2CM_INTFLAG_MB_Msk |
                             SERCOM_I2CM_INTFLAG_SB_Msk)) != 0U) {
      break;
    }
  }
  if (guard == 0U || !i2c_command(s, I2C_COMMAND_STOP, nack)) {
    return false;
  }
  /* SYSOP means synchronization, not completion on the wire. Await bus release. */
  for (guard = I2C_SYNC_LIMIT; guard != 0U; --guard) {
    if (i2c_bus_state(s) != I2C_BUS_OWNER) {
      return true;
    }
  }
  return false;
}

static bool i2c_hardware_init (I2C_STATE *s) {
  sercom_i2cm_registers_t *r = s->pins->regs;
  r->SERCOM_CTRLA = SERCOM_I2CM_CTRLA_SWRST_Msk;
  if (!i2c_sync(s) || (r->SERCOM_CTRLA & SERCOM_I2CM_CTRLA_SWRST_Msk) != 0U) {
    return false;
  }
  r->SERCOM_CTRLA = SERCOM_I2CM_CTRLA_MODE_I2C_MASTER |
                    SERCOM_I2CM_CTRLA_SDAHOLD(2U);
  /* Smart mode is deliberately off: ACK/NACK and next-byte commands are explicit. */
  r->SERCOM_CTRLB = 0U;
  r->SERCOM_BAUD = SERCOM_I2CM_BAUD_BAUD(s->baud) |
                   SERCOM_I2CM_BAUD_BAUDLOW(s->baud_low);
  r->SERCOM_INTENCLR = I2C_IRQ_FLAGS;
  r->SERCOM_CTRLA |= SERCOM_I2CM_CTRLA_ENABLE_Msk;
  if (!i2c_sync(s)) {
    return false;
  }
  r->SERCOM_STATUS = SERCOM_I2CM_STATUS_BUSSTATE(I2C_BUS_IDLE);
  return i2c_sync(s);
}

static ARM_DRIVER_VERSION i2c_version (void) {
  ARM_DRIVER_VERSION version = { ARM_I2C_API_VERSION, ARM_DRIVER_VERSION_MAJOR_MINOR(1, 0) };
  return version;
}

static ARM_I2C_CAPABILITIES i2c_capabilities (void) {
  ARM_I2C_CAPABILITIES capabilities = {0};
  /* 7-bit master only; ten-bit addressing and slave operation are unsupported. */
  return capabilities;
}

static int32_t i2c_initialize (I2C_STATE *s, ARM_I2C_SignalEvent_t callback) {
  if (s->initialized) {
    return ARM_DRIVER_OK;
  }
  s->callback = callback;
  /* BAUDLOW=0 selects symmetric timing; zero rise-time gives a conservative rate. */
  s->baud = (uint8_t)(I2C_CLOCK_HZ / (2U * 100000U) - 5U);
  s->baud_low = 0U;
  s->count = 0U;
  s->initialized = true;
  return ARM_DRIVER_OK;
}

static int32_t i2c_power (I2C_STATE *s, ARM_POWER_STATE state) {
  sercom_i2cm_registers_t *r = s->pins->regs;
  if (state == ARM_POWER_LOW) {
    return ARM_DRIVER_ERROR_UNSUPPORTED;
  }
  if (state == ARM_POWER_OFF) {
    bool synced = true;
    i2c_nvic(s, false);
    if (s->powered) {
      r->SERCOM_INTENCLR = I2C_IRQ_FLAGS;
      synced = i2c_stop(s, true);
      r->SERCOM_CTRLA &= ~SERCOM_I2CM_CTRLA_ENABLE_Msk;
      synced = i2c_sync(s) && synced;
      wio_gpio_input(s->pins->sda, 0);
      wio_gpio_input(s->pins->scl, 0);
      *s->pins->clock_mask &= ~s->pins->clock_bit;
    }
    s->busy = false;
    s->held = false;
    s->powered = false;
    s->faulted = false;
    return synced ? ARM_DRIVER_OK : ARM_DRIVER_ERROR_TIMEOUT;
  }
  if (state != ARM_POWER_FULL) {
    return ARM_DRIVER_ERROR_PARAMETER;
  }
  if (!s->initialized) {
    return ARM_DRIVER_ERROR;
  }
  if (s->powered) {
    return s->faulted ? ARM_DRIVER_ERROR : ARM_DRIVER_OK;
  }
  *s->pins->clock_mask |= s->pins->clock_bit;
  if (!wio_gclk_enable(s->pins->clock_id)) {
    *s->pins->clock_mask &= ~s->pins->clock_bit;
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  i2c_nvic(s, false);
  /* SERCOM disables internal pulls in I2C mode; the bus needs external pull-ups. */
  wio_gpio_input(s->pins->sda, 0);
  wio_gpio_input(s->pins->scl, 0);
  wio_gpio_function(s->pins->sda, 3U);
  wio_gpio_function(s->pins->scl, 3U);
  if (!i2c_hardware_init(s)) {
    wio_gpio_input(s->pins->sda, 0);
    wio_gpio_input(s->pins->scl, 0);
    *s->pins->clock_mask &= ~s->pins->clock_bit;
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  s->powered = true;
  s->faulted = false;
  s->busy = false;
  s->held = false;
  s->count = 0U;
  s->arbitration_lost = false;
  s->bus_error = false;
  i2c_nvic(s, true);
  return ARM_DRIVER_OK;
}

static int32_t i2c_uninitialize (I2C_STATE *s) {
  int32_t result = i2c_power(s, ARM_POWER_OFF);
  s->initialized = false;
  s->callback = NULL;
  return result;
}

static int32_t i2c_abort (I2C_STATE *s) {
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  sercom_i2cm_registers_t *r = s->pins->regs;
  r->SERCOM_INTENCLR = I2C_IRQ_FLAGS;
  /* Synchronize even if ADDR has not yet changed BUSSTATE from IDLE to OWNER. */
  bool synced = i2c_stop(s, true);
  if (!synced || s->faulted) {
    /* A stalled peripheral must not leave the software permanently busy. */
    synced = i2c_hardware_init(s);
  } else {
    i2c_clear_errors(s);
    r->SERCOM_INTFLAG = SERCOM_I2CM_INTFLAG_MB_Msk | SERCOM_I2CM_INTFLAG_SB_Msk;
  }
  s->held = false;
  s->faulted = !synced;
  s->busy = false;
  __DMB();
  __set_PRIMASK(primask);
  return synced ? ARM_DRIVER_OK : ARM_DRIVER_ERROR_TIMEOUT;
}

static int32_t i2c_control (I2C_STATE *s, uint32_t control, uint32_t arg) {
  if (!s->powered) {
    return ARM_DRIVER_ERROR;
  }
  if (control == ARM_I2C_ABORT_TRANSFER) {
    return i2c_abort(s);
  }
  if (s->busy || s->held) {
    return ARM_DRIVER_ERROR_BUSY;
  }
  if (control != ARM_I2C_BUS_SPEED) {
    /* No slave-own-address or GPIO bus-clear emulation. */
    return ARM_DRIVER_ERROR_UNSUPPORTED;
  }
  uint8_t baud, baud_low;
  switch (arg) {
    case ARM_I2C_BUS_SPEED_STANDARD:
      baud = (uint8_t)(I2C_CLOCK_HZ / (2U * 100000U) - 5U);
      baud_low = 0U;
      break;
    case ARM_I2C_BUS_SPEED_FAST:
      /* 48 MHz: 63 low cycles (1.3125 us) + 57 high cycles = 400 kHz maximum.
       * A symmetric 1.25 us low period would violate Fast-mode's 1.3 us minimum.
       * External rise time only reduces the actual frequency (p.947).
       */
      baud = 52U;
      baud_low = 58U;
      break;
    default: return ARM_DRIVER_ERROR_UNSUPPORTED;
  }
  sercom_i2cm_registers_t *r = s->pins->regs;
  r->SERCOM_CTRLA &= ~SERCOM_I2CM_CTRLA_ENABLE_Msk;
  if (!i2c_sync(s)) {
    s->faulted = true;
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  s->baud = baud;
  s->baud_low = baud_low;
  r->SERCOM_BAUD = SERCOM_I2CM_BAUD_BAUD(s->baud) |
                   SERCOM_I2CM_BAUD_BAUDLOW(s->baud_low);
  r->SERCOM_CTRLA |= SERCOM_I2CM_CTRLA_ENABLE_Msk;
  if (!i2c_sync(s)) {
    s->faulted = true;
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  r->SERCOM_STATUS = SERCOM_I2CM_STATUS_BUSSTATE(I2C_BUS_IDLE);
  if (!i2c_sync(s)) {
    s->faulted = true;
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  s->faulted = false;
  return ARM_DRIVER_OK;
}

static int32_t i2c_transfer (I2C_STATE *s, uint32_t addr, const uint8_t *tx,
                             uint8_t *rx, uint32_t num, bool pending) {
  if (addr > 0x7FU || num == 0U || num > (uint32_t)INT32_MAX ||
      (tx == NULL && rx == NULL)) {
    return ARM_DRIVER_ERROR_PARAMETER;
  }
  if (!s->powered || s->faulted) {
    return ARM_DRIVER_ERROR;
  }
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (s->busy || (s->held ? i2c_bus_state(s) != I2C_BUS_OWNER :
                             i2c_bus_state(s) != I2C_BUS_IDLE)) {
    __set_PRIMASK(primask);
    return ARM_DRIVER_ERROR_BUSY;
  }
  if (!s->held && !i2c_command(s, 0U, false)) {
    s->faulted = true;
    __set_PRIMASK(primask);
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  sercom_i2cm_registers_t *r = s->pins->regs;
  r->SERCOM_INTENCLR = I2C_IRQ_FLAGS;
  i2c_clear_errors(s);
  if (!i2c_sync(s)) {
    s->faulted = true;
    __set_PRIMASK(primask);
    return ARM_DRIVER_ERROR_TIMEOUT;
  }
  s->tx = tx;
  s->rx = rx;
  s->total = num;
  s->sent = 0U;
  s->count = 0U;
  s->receive = (rx != NULL);
  s->pending = pending;
  s->held = false;
  s->arbitration_lost = false;
  s->bus_error = false;
  s->busy = true;
  __DMB();
  /* While OWNER, writing ADDR completes the previous ACK/NACK and repeats START. */
  r->SERCOM_ADDR = (addr << 1) | (s->receive ? 1U : 0U);
  r->SERCOM_INTENSET = I2C_IRQ_FLAGS;
  __set_PRIMASK(primask);
  return ARM_DRIVER_OK;
}

static ARM_I2C_STATUS i2c_status (I2C_STATE *s) {
  ARM_I2C_STATUS status = {0};
  status.busy = s->busy;
  status.mode = 1U;
  status.direction = s->receive;
  status.arbitration_lost = s->arbitration_lost;
  status.bus_error = s->bus_error;
  return status;
}

static void i2c_finish (I2C_STATE *s, uint32_t events) {
  sercom_i2cm_registers_t *r = s->pins->regs;
  r->SERCOM_INTENCLR = I2C_IRQ_FLAGS;
  events |= i2c_latch_errors(s);
  bool success = (events == 0U);
  if (success && s->pending) {
    /* Retain MB/SB and clock stretching until the next ADDR write. */
    s->held = true;
  } else {
    s->held = false;
    if (i2c_bus_state(s) == I2C_BUS_OWNER && !i2c_stop(s, s->receive)) {
      s->faulted = true;
      s->bus_error = true;
      events |= ARM_I2C_EVENT_BUS_ERROR;
    }
    /* Sending the final NACK/STOP can itself lose arbitration. */
    events |= i2c_latch_errors(s);
    i2c_clear_errors(s);
    r->SERCOM_INTFLAG = SERCOM_I2CM_INTFLAG_MB_Msk | SERCOM_I2CM_INTFLAG_SB_Msk;
  }
  if (events != 0U || s->count != s->total) {
    events |= ARM_I2C_EVENT_TRANSFER_INCOMPLETE;
  }
  __DMB();
  s->busy = false;
  if (s->callback != NULL) {
    s->callback(events | ARM_I2C_EVENT_TRANSFER_DONE);
  }
}

static void i2c_interrupt (I2C_STATE *s) {
  sercom_i2cm_registers_t *r = s->pins->regs;
  if (!s->powered) {
    return;
  }
  if (!s->busy) {
    r->SERCOM_INTENCLR = I2C_IRQ_FLAGS;
    return;
  }
  uint32_t flags = r->SERCOM_INTFLAG & r->SERCOM_INTENSET;
  uint32_t status = r->SERCOM_STATUS;
  uint32_t errors = i2c_latch_errors(s);
  if (errors != 0U) {
    i2c_finish(s, errors);
    return;
  }
  if ((flags & SERCOM_I2CM_INTFLAG_MB_Msk) != 0U) {
    if ((status & SERCOM_I2CM_STATUS_RXNACK_Msk) != 0U) {
      uint32_t cause = (s->sent == 0U) ? ARM_I2C_EVENT_ADDRESS_NACK :
                       ARM_I2C_EVENT_TRANSFER_INCOMPLETE;
      i2c_finish(s, cause);
      return;
    }
    if (s->receive) {
      s->bus_error = true;
      i2c_finish(s, ARM_I2C_EVENT_BUS_ERROR);
      return;
    }
    /* MB follows the address ACK, then each acknowledged data byte. */
    s->count = s->sent;
    if (s->count == s->total) {
      i2c_finish(s, 0U);
      return;
    }
    r->SERCOM_DATA = s->tx[s->sent++];
    return;
  }
  if ((flags & SERCOM_I2CM_INTFLAG_SB_Msk) != 0U && s->receive) {
    bool last = (s->count + 1U == s->total);
    if (!i2c_command(s, 0U, last)) {
      s->faulted = true;
      s->bus_error = true;
      i2c_finish(s, ARM_I2C_EVENT_BUS_ERROR);
      return;
    }
    s->rx[s->count++] = (uint8_t)r->SERCOM_DATA;
    if (last) {
      i2c_finish(s, 0U);
    } else if (!i2c_command(s, I2C_COMMAND_READ, false)) {
      s->faulted = true;
      s->bus_error = true;
      i2c_finish(s, ARM_I2C_EVENT_BUS_ERROR);
    }
  }
}

static int32_t i2c_slave_transmit (const uint8_t *data, uint32_t num) {
  (void)data; (void)num;
  return ARM_DRIVER_ERROR_UNSUPPORTED;
}

static int32_t i2c_slave_receive (uint8_t *data, uint32_t num) {
  (void)data; (void)num;
  return ARM_DRIVER_ERROR_UNSUPPORTED;
}

#define I2C_INSTANCE(n) \
  static int32_t I2C##n##_Initialize(ARM_I2C_SignalEvent_t cb) { return i2c_initialize(&i2c##n, cb); } \
  static int32_t I2C##n##_Uninitialize(void) { return i2c_uninitialize(&i2c##n); } \
  static int32_t I2C##n##_PowerControl(ARM_POWER_STATE p) { return i2c_power(&i2c##n, p); } \
  static int32_t I2C##n##_MasterTransmit(uint32_t a, const uint8_t *d, uint32_t count, bool p) { \
    return d != NULL ? i2c_transfer(&i2c##n, a, d, NULL, count, p) : ARM_DRIVER_ERROR_PARAMETER; \
  } \
  static int32_t I2C##n##_MasterReceive(uint32_t a, uint8_t *d, uint32_t count, bool p) { \
    return d != NULL ? i2c_transfer(&i2c##n, a, NULL, d, count, p) : ARM_DRIVER_ERROR_PARAMETER; \
  } \
  static int32_t I2C##n##_GetDataCount(void) { return (int32_t)i2c##n.count; } \
  static int32_t I2C##n##_Control(uint32_t c, uint32_t a) { return i2c_control(&i2c##n, c, a); } \
  static ARM_I2C_STATUS I2C##n##_GetStatus(void) { return i2c_status(&i2c##n); } \
  ARM_DRIVER_I2C Driver_I2C##n = { i2c_version, i2c_capabilities, \
    I2C##n##_Initialize, I2C##n##_Uninitialize, I2C##n##_PowerControl, \
    I2C##n##_MasterTransmit, I2C##n##_MasterReceive, i2c_slave_transmit, i2c_slave_receive, \
    I2C##n##_GetDataCount, I2C##n##_Control, I2C##n##_GetStatus }; \
  void SERCOM##n##_0_Handler(void) { i2c_interrupt(&i2c##n); } \
  void SERCOM##n##_1_Handler(void) { i2c_interrupt(&i2c##n); } \
  void SERCOM##n##_2_Handler(void) { i2c_interrupt(&i2c##n); } \
  void SERCOM##n##_OTHER_Handler(void) { i2c_interrupt(&i2c##n); }

I2C_INSTANCE(3)
I2C_INSTANCE(4)
