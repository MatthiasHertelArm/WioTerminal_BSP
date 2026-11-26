/* SPDX-License-Identifier: Apache-2.0 */
/* CMSIS-only transport; deliberately never changes RTL8720DN power/boot pins. */
#include "Driver_USART.h"
#include "wio_network_port.h"
#include "wio_sdk.h"

extern ARM_DRIVER_USART Driver_USART0;

#define RX_RING_SIZE 8192U
#define RX_RING_MASK (RX_RING_SIZE - 1U)
static uint8_t rx_ring[RX_RING_SIZE];
static uint8_t rx_byte;
static volatile uint32_t rx_head, rx_tail;
static volatile bool active, failed;
static bool initialized;
static uint32_t tx_length;

static void uart_event(uint32_t events) {
  if (!active) {
    return;
  }
  if ((events & (ARM_USART_EVENT_RX_OVERFLOW |
                 ARM_USART_EVENT_RX_FRAMING_ERROR |
                 ARM_USART_EVENT_RX_PARITY_ERROR |
                 ARM_USART_EVENT_RX_BREAK |
                 ARM_USART_EVENT_TX_UNDERFLOW)) != 0U) {
    failed = true;
  }
  if ((events & ARM_USART_EVENT_RECEIVE_COMPLETE) != 0U && !failed) {
    uint32_t next = (rx_head + 1U) & RX_RING_MASK;
    if (next == rx_tail) {
      failed = true;
      return;
    }
    rx_ring[rx_head] = rx_byte;
    __DMB();
    rx_head = next;
    /* Rearm here, not later in foreground: bytes arrive only 16.3 us apart.
     * The CMSIS driver publishes receive completion before this callback.
     */
    if (Driver_USART0.Receive(&rx_byte, 1U) != ARM_DRIVER_OK) {
      failed = true;
    }
  }
}

bool wio_network_port_thread(void) {
  return __get_IPSR() == 0U && __get_PRIMASK() == 0U &&
         __get_BASEPRI() == 0U && __get_FAULTMASK() == 0U;
}

uint32_t wio_network_port_millis(void) { return wio_millis(); }
void wio_network_port_idle(void) { __NOP(); }

void wio_network_port_close(void) {
  if (!initialized) {
    return;
  }
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  active = false;
  __set_PRIMASK(primask);
  (void)Driver_USART0.Control(ARM_USART_ABORT_RECEIVE, 0U);
  (void)Driver_USART0.Control(ARM_USART_ABORT_SEND, 0U);
  (void)Driver_USART0.PowerControl(ARM_POWER_OFF);
  (void)Driver_USART0.Uninitialize();
  initialized = false;
  tx_length = 0U;
}

bool wio_network_port_open(void) {
  if (initialized || !wio_network_port_thread()) {
    return false;
  }
  rx_head = 0U;
  rx_tail = 0U;
  failed = false;
  active = false;
  tx_length = 0U;
  /* Caller must not independently initialize USART0: CMSIS initialization is
   * idempotent and an existing owner's callback must not be overwritten.
   */
  if (Driver_USART0.Initialize(uart_event) != ARM_DRIVER_OK) {
    return false;
  }
  initialized = true;
  if (Driver_USART0.PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK ||
      Driver_USART0.Control(ARM_USART_MODE_ASYNCHRONOUS |
                            ARM_USART_DATA_BITS_8 | ARM_USART_PARITY_NONE |
                            ARM_USART_STOP_BITS_1 | ARM_USART_FLOW_CONTROL_NONE,
                            614400U) != ARM_DRIVER_OK ||
      Driver_USART0.Control(ARM_USART_CONTROL_TX, 1U) != ARM_DRIVER_OK) {
    wio_network_port_close();
    return false;
  }
  /* Enable RX and provide its first buffer atomically; otherwise an incoming
   * boot event could be consumed by the IRQ before Receive has been armed.
   */
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  active = true;
  bool ok = Driver_USART0.Control(ARM_USART_CONTROL_RX, 1U) == ARM_DRIVER_OK &&
            Driver_USART0.Receive(&rx_byte, 1U) == ARM_DRIVER_OK;
  __set_PRIMASK(primask);
  if (!ok) {
    wio_network_port_close();
  }
  return ok;
}

bool wio_network_port_error(void) { return !active || failed; }

int32_t wio_network_port_read(uint8_t *data, size_t capacity) {
  if (wio_network_port_error()) {
    return -1;
  }
  size_t count = 0U;
  while (count < capacity && rx_tail != rx_head) {
    __DMB();
    data[count++] = rx_ring[rx_tail];
    __DMB();
    rx_tail = (rx_tail + 1U) & RX_RING_MASK;
  }
  return failed ? -1 : (int32_t)count;
}

bool wio_network_port_send(const uint8_t *data, size_t length) {
  if (wio_network_port_error() || data == NULL || length == 0U || length > 4100U) {
    return false;
  }
  tx_length = (uint32_t)length;
  return Driver_USART0.Send(data, tx_length) == ARM_DRIVER_OK;
}

bool wio_network_port_tx_done(void) {
  if (Driver_USART0.GetStatus().tx_busy != 0U) {
    return false;
  }
  if (Driver_USART0.GetTxCount() != tx_length) {
    failed = true;
  }
  return true;
}
