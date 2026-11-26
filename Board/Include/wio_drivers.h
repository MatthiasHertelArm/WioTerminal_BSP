/* SPDX-License-Identifier: Apache-2.0 */
/* CMSIS-Driver instances provided by the Wio Terminal BSP. */
#ifndef WIO_DRIVERS_H_
#define WIO_DRIVERS_H_

#include "Driver_I2C.h"
#include "Driver_SPI.h"
#include "Driver_USART.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize and power only the instances your application needs. */
extern ARM_DRIVER_I2C Driver_I2C3;  /* Grove / header I2C. */
extern ARM_DRIVER_I2C Driver_I2C4;  /* On-board LIS3DH accelerometer. */

extern ARM_DRIVER_SPI Driver_SPI0; /* RTL8720DN transport; no radio protocol. */
extern ARM_DRIVER_SPI Driver_SPI5; /* External 40-pin-header SPI. */
extern ARM_DRIVER_SPI Driver_SPI6; /* microSD socket, SPI mode. */
extern ARM_DRIVER_SPI Driver_SPI7; /* ILI9341 LCD. */

extern ARM_DRIVER_USART Driver_USART0; /* RTL8720DN eRPC; mutually exclusive with SPI0. */
extern ARM_DRIVER_USART Driver_USART1; /* RTL8720DN programming/log UART transport. */
extern ARM_DRIVER_USART Driver_USART2; /* 40-pin header, pins 8 and 10. */

/*
 * These drivers are interrupt-driven, master-only, and have no DMA dependency.
 * SPI supports 8-bit Motorola frames, MSB/LSB first, and unused/software SS.
 * I2C supports 7-bit addresses, 100/400 kHz, and xfer_pending repeated starts.
 * I2C needs external pull-ups: SERCOM disables the internal pulls in I2C mode.
 * Callbacks run in interrupt context; buffers must survive until completion.
 * Abort a timed-out transfer with Control(..._ABORT_TRANSFER, 0).
 */

#ifdef __cplusplus
}
#endif

#endif /* WIO_DRIVERS_H_ */
