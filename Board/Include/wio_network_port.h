/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WIO_NETWORK_PORT_H_
#define WIO_NETWORK_PORT_H_
/* Internal platform boundary. Applications use wio_network.h instead.
 * Unit tests replace these functions without including any MCU headers.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool wio_network_port_thread(void);
uint32_t wio_network_port_millis(void);
void wio_network_port_idle(void);
/* open/close must not change radio power/reset/boot pins. */
bool wio_network_port_open(void);
void wio_network_port_close(void);
/* read is nonblocking. Returns bytes read, or -1 on any sticky UART/ring error. */
int32_t wio_network_port_read(uint8_t *data, size_t capacity);
/* The engine owns data until tx_done or close. Never point DMA/IRQ at stack. */
bool wio_network_port_send(const uint8_t *data, size_t length);
bool wio_network_port_tx_done(void);
bool wio_network_port_error(void);
#endif
