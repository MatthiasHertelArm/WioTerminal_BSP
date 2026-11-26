/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WIO_FLASH_H_
#define WIO_FLASH_H_

#include <stdbool.h>
#include <stdint.h>
#include "Driver_Flash.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WIO_FLASH_SIZE_BYTES  0x00400000UL
#define WIO_FLASH_JEDEC_IQ    0x00EF4016UL
#define WIO_FLASH_JEDEC_IM    0x00EF7016UL

/*
 * Conservative READ-ONLY CMSIS Flash interface for the on-board W25Q32JV.
 * Uses standard single-lane, mode-0 software SPI on the QSPI pads, at no more
 * than 500 kHz. This is not a native QSPI, memory-mapped, or XIP driver.
 *
 * Call wio_sdk_init(), Initialize(NULL), then PowerControl(ARM_POWER_FULL).
 * This interface exclusively owns all six flash pins: do not use the native
 * QSPI peripheral, mapped flash window, or another pin owner at the same time.
 * The device must already be in standard SPI/standby mode. This driver never
 * sends write-enable, status-write, reset, wake-up, program, or erase commands.
 *
 * ReadData uses zero-based flash offsets, not CPU addresses. It is synchronous
 * and returns the number of bytes read, or a negative ARM_DRIVER_ERROR_* value.
 * It checks JEDEC identity on every read and supports EF 40 16 and EF 70 16 only.
 * Unknown IDs return ARM_DRIVER_ERROR_UNSUPPORTED. Pre-existing chip BUSY or
 * suspend states return ARM_DRIVER_ERROR_BUSY without waiting or altering them.
 * Zero-sized, NULL, and out-of-range reads return ARM_DRIVER_ERROR_PARAMETER.
 * On an aborted read the destination may contain a partial result.
 *
 * GetStatus reports the driver's current transfer and last read error. Calls
 * are serialized; PowerControl(OFF)/Uninitialize can cancel a transfer. Short
 * critical sections protect each SPI bit, with interrupts restored between bits.
 * Large reads block the caller; prefer small chunks in a responsive application.
 * event_ready=0: no callback is generated, and buffers are unused after return.
 *
 * GetInfo describes the supported chip's geometry, not write capability.
 * ProgramData, EraseSector and EraseChip ALWAYS return
 * ARM_DRIVER_ERROR_UNSUPPORTED without touching the pins or flash contents.
 */
extern ARM_DRIVER_FLASH Driver_Flash0;

/*
 * Read three raw JEDEC bytes after Initialize/PowerControl(FULL).
 * Output is changed only on success. A true result means samples were read,
 * not that the ID is supported or that stored contents have been verified.
 */
bool wio_flash_read_id(uint8_t id[3]);

#ifdef __cplusplus
}
#endif

#endif /* WIO_FLASH_H_ */
