/* SPDX-License-Identifier: Apache-2.0 */
/* Compile the actual UART adapter against the genuine CMSIS USART interface. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "Driver_USART.h"
#include "wio_network_port.h"
#include "wio_sdk.h"

uint32_t test_primask, test_ipsr, test_basepri, test_faultmask;
static uint32_t ticks;
uint32_t wio_millis(void) { return ticks; }
static struct {
  ARM_USART_SignalEvent_t callback;
  uint8_t *rx;
  const uint8_t *tx;
  uint32_t tx_count, tx_length, receive_calls, power_offs, uninitializes;
  bool initialized, powered, tx_enabled, rx_enabled, tx_busy;
  bool reject_init, reject_power, reject_receive, reject_send;
} uart;
static unsigned assertions;
#define CHECK(expression) do { ++assertions; assert(expression); } while (0)

static int32_t initialize(ARM_USART_SignalEvent_t callback) {
  CHECK(!uart.initialized);
  if (uart.reject_init) { return ARM_DRIVER_ERROR; }
  uart.initialized = true;
  uart.callback = callback;
  return ARM_DRIVER_OK;
}
static int32_t uninitialize(void) {
  uart.initialized = false; uart.callback = NULL;
  ++uart.uninitializes;
  return ARM_DRIVER_OK;
}
static int32_t power(ARM_POWER_STATE state) {
  if (state == ARM_POWER_FULL) {
    CHECK(uart.initialized);
    if (uart.reject_power) { return ARM_DRIVER_ERROR_BUSY; }
    uart.powered = true;
  } else {
    CHECK(state == ARM_POWER_OFF);
    uart.powered = false; uart.rx = NULL; uart.tx = NULL;
    uart.tx_busy = false; uart.tx_enabled = false; uart.rx_enabled = false;
    ++uart.power_offs;
  }
  return ARM_DRIVER_OK;
}
static int32_t control(uint32_t operation, uint32_t argument) {
  switch (operation) {
    case ARM_USART_ABORT_RECEIVE: uart.rx = NULL; return ARM_DRIVER_OK;
    case ARM_USART_ABORT_SEND: uart.tx = NULL; uart.tx_busy = false; return ARM_DRIVER_OK;
    case ARM_USART_CONTROL_TX: uart.tx_enabled = argument != 0U; return ARM_DRIVER_OK;
    case ARM_USART_CONTROL_RX:
      CHECK(test_primask != 0U); /* RX enable + initial Receive are atomic. */
      uart.rx_enabled = argument != 0U;
      return ARM_DRIVER_OK;
    default:
      CHECK(operation == (ARM_USART_MODE_ASYNCHRONOUS | ARM_USART_DATA_BITS_8 |
                           ARM_USART_PARITY_NONE | ARM_USART_STOP_BITS_1 |
                           ARM_USART_FLOW_CONTROL_NONE));
      CHECK(argument == 614400U && uart.powered);
      return ARM_DRIVER_OK;
  }
}
static int32_t receive(void *data, uint32_t count) {
  CHECK(uart.rx == NULL && data != NULL && count == 1U && uart.rx_enabled);
  ++uart.receive_calls;
  if (uart.reject_receive) { return ARM_DRIVER_ERROR; }
  uart.rx = data;
  return ARM_DRIVER_OK;
}
static int32_t send(const void *data, uint32_t count) {
  CHECK(uart.tx_enabled && data != NULL && count != 0U);
  if (uart.reject_send) { return ARM_DRIVER_ERROR; }
  uart.tx = data; uart.tx_length = count; uart.tx_count = 0U; uart.tx_busy = true;
  return ARM_DRIVER_OK;
}
static ARM_USART_STATUS status(void) {
  ARM_USART_STATUS result = {0};
  result.tx_busy = uart.tx_busy;
  result.rx_busy = uart.rx != NULL;
  return result;
}
static uint32_t tx_count(void) { return uart.tx_count; }
ARM_DRIVER_USART Driver_USART0 = {
  .Initialize = initialize, .Uninitialize = uninitialize, .PowerControl = power,
  .Send = send, .Receive = receive, .GetTxCount = tx_count,
  .Control = control, .GetStatus = status
};

static void receive_byte(uint8_t value, uint32_t extra_events) {
  CHECK(uart.rx != NULL && uart.callback != NULL);
  *uart.rx = value;
  uart.rx = NULL;
  test_ipsr = 1U;
  uart.callback(ARM_USART_EVENT_RECEIVE_COMPLETE | extra_events);
  test_ipsr = 0U;
}
static void reset(void) {
  wio_network_port_close();
  CHECK(uart.rx == NULL && uart.tx == NULL);
  memset(&uart, 0, sizeof(uart));
  test_primask = test_ipsr = test_basepri = test_faultmask = 0U;
}
static void test_context_and_open(void) {
  reset();
  CHECK(wio_network_port_thread());
  test_ipsr = 1U; CHECK(!wio_network_port_thread() && !wio_network_port_open()); test_ipsr = 0U;
  test_primask = 1U; CHECK(!wio_network_port_thread()); test_primask = 0U;
  test_basepri = 1U; CHECK(!wio_network_port_thread()); test_basepri = 0U;
  test_faultmask = 1U; CHECK(!wio_network_port_thread()); test_faultmask = 0U;
  CHECK(wio_network_port_open() && uart.receive_calls == 1U && uart.rx != NULL);
  CHECK(!wio_network_port_open());
  CHECK(test_primask == 0U && !wio_network_port_error());
  ticks = 123U; CHECK(wio_network_port_millis() == 123U);
  wio_network_port_idle();
  wio_network_port_close();
  CHECK(uart.power_offs == 1U && uart.uninitializes == 1U && uart.rx == NULL);
  CHECK(wio_network_port_error());
  uint8_t byte;
  CHECK(wio_network_port_read(&byte, 1U) == -1);
  CHECK(!wio_network_port_send(&byte, 1U));
}
static void test_ring_wrap_and_tx(void) {
  reset(); CHECK(wio_network_port_open());
  uint8_t bytes[127];
  for (unsigned block = 0U; block < 150U; ++block) {
    for (size_t i = 0U; i < sizeof(bytes); ++i) { receive_byte((uint8_t)(block + i), 0U); }
    CHECK(wio_network_port_read(bytes, sizeof(bytes)) == (int32_t)sizeof(bytes));
    for (size_t i = 0U; i < sizeof(bytes); ++i) { CHECK(bytes[i] == (uint8_t)(block + i)); }
    CHECK(wio_network_port_read(bytes, sizeof(bytes)) == 0);
  }
  CHECK(!wio_network_port_send(NULL, 3U));
  CHECK(!wio_network_port_send(bytes, 0U));
  CHECK(!wio_network_port_send(bytes, 4101U));
  CHECK(wio_network_port_send(bytes, sizeof(bytes)));
  CHECK(!wio_network_port_tx_done());
  uart.tx_count = uart.tx_length; uart.tx_busy = false;
  CHECK(wio_network_port_tx_done() && !wio_network_port_error());
  CHECK(wio_network_port_send(bytes, sizeof(bytes)));
  uart.tx_count = 2U; uart.tx_busy = false;
  CHECK(wio_network_port_tx_done() && wio_network_port_error());
}
static void test_sticky_errors(void) {
  const uint32_t errors[] = {ARM_USART_EVENT_RX_OVERFLOW, ARM_USART_EVENT_RX_FRAMING_ERROR,
    ARM_USART_EVENT_RX_PARITY_ERROR, ARM_USART_EVENT_RX_BREAK, ARM_USART_EVENT_TX_UNDERFLOW};
  uint8_t byte = 0U;
  for (size_t i = 0U; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    reset(); CHECK(wio_network_port_open());
    receive_byte(0xA5U, errors[i]);
    CHECK(wio_network_port_error() && uart.rx == NULL);
    CHECK(wio_network_port_read(&byte, 1U) == -1);
    CHECK(byte == 0U);
  }
  reset(); CHECK(wio_network_port_open());
  for (unsigned i = 0U; i < 8192U; ++i) { receive_byte((uint8_t)i, 0U); }
  CHECK(wio_network_port_error() && uart.rx == NULL);
  CHECK(wio_network_port_read(&byte, 1U) == -1);
  reset(); CHECK(wio_network_port_open());
  uart.reject_receive = true; receive_byte(1U, 0U);
  CHECK(wio_network_port_error() && uart.rx == NULL);
}
static void test_open_failures(void) {
  reset(); uart.reject_init = true;
  CHECK(!wio_network_port_open() && !uart.initialized);
  reset(); uart.reject_power = true;
  CHECK(!wio_network_port_open() && !uart.initialized && !uart.powered);
  reset(); uart.reject_receive = true;
  CHECK(!wio_network_port_open() && !uart.initialized && !uart.powered && uart.rx == NULL);
  CHECK(test_primask == 0U);
  reset(); CHECK(wio_network_port_open()); uart.reject_send = true;
  const uint8_t byte = 1U;
  CHECK(!wio_network_port_send(&byte, 1U));
  wio_network_port_close();
  CHECK(uart.rx == NULL && uart.tx == NULL);
}
int main(void) {
  test_context_and_open();
  test_ring_wrap_and_tx();
  test_sticky_errors();
  test_open_failures();
  reset();
  printf("network UART: %u assertions passed\n", assertions);
  return 0;
}
