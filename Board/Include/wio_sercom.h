/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WIO_SERCOM_H_
#define WIO_SERCOM_H_

#include <stdbool.h>

/* Internal driver resource guard. SERCOM0 shares radio SPI and eRPC UART pins.
 * A driver acquires it before changing clocks/registers, and releases it only
 * after disabling all IRQs and the peripheral clock. No application calls.
 */
typedef void (*wio_sercom_irq_t)(void);
bool wio_sercom0_acquire(wio_sercom_irq_t handler);
void wio_sercom0_release(wio_sercom_irq_t handler);

/* Initialize, PowerControl and Uninitialize calls must be externally serialized
 * in foreground context, like the SDK's blocking peripheral operations. */
#endif
