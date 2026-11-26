/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WIO_SD_H_
#define WIO_SD_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIO_SD_SECTOR_SIZE 512U

typedef enum {
  WIO_SD_OK = 0,
  WIO_SD_PARAMETER,
  WIO_SD_NOT_READY,
  WIO_SD_WRITE_PROTECTED,
  WIO_SD_TIMEOUT,
  WIO_SD_IO,
  WIO_SD_CRC,
  WIO_SD_UNSUPPORTED,
  WIO_SD_BUSY
} wio_sd_result_t;

typedef struct {
  uint64_t sector_count;
  uint32_t sector_size;
  bool block_addressed;       /* SDHC/SDXC: command addresses are sectors. */
  bool write_protected;       /* Permanent/temporary protection in CSD. */
  bool writable;              /* Explicit software write gate, initially false. */
  uint8_t csd[16];
} wio_sd_info_t;

/* Blocking, non-reentrant, bare-metal SD SPI transport; exclusively owns
 * Driver_SPI6 and the microSD pins. Call wio_sdk_init() first and keep IRQs
 * enabled. Call only from thread mode and serialize all calls. Do not use this
 * API directly while wio_fs owns a mounted volume.
 *
 * Supports SD v1/v2, SDSC and SDHC/SDXC with CSD v1/v2. MMC and SDUC are not
 * supported. Initialization uses <=400 kHz, data <=4 MHz. Command/data CRCs
 * are checked. No format, trim, erase, automatic retry of writes, or hot-swap
 * detection is provided. Unmount/deinitialize before removing a card.
 *
 * init/deinit and a transport/media failure close the write gate. A failed
 * multi-sector operation may have transferred earlier sectors; writes are not
 * atomic. A write timeout can mean the card programmed data before responding.
 */
wio_sd_result_t wio_sd_init(void);
wio_sd_result_t wio_sd_deinit(void);
wio_sd_result_t wio_sd_get_info(wio_sd_info_t *info);
wio_sd_result_t wio_sd_set_writable(bool writable);
wio_sd_result_t wio_sd_read_blocks(uint32_t lba, void *buffer, uint32_t count);
wio_sd_result_t wio_sd_write_blocks(uint32_t lba, const void *buffer,
                                    uint32_t count);
wio_sd_result_t wio_sd_sync(void);

#ifdef __cplusplus
}
#endif
#endif
