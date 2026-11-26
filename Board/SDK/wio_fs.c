/* SPDX-License-Identifier: Apache-2.0 */
#include "wio_fs.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "diskio.h"
#include "wio_sd.h"
#include "wio_terminal.h"

#if FF_FS_READONLY || (FF_FS_LOCK < 1) || (FF_VOLUMES != 1) || FF_LBA64 || \
    (FF_MIN_SS != 512) || (FF_MAX_SS != 512) || FF_FS_REENTRANT || FF_USE_MKFS || \
    FF_USE_TRIM
#error Review the Wio filesystem bridge before changing its FatFs profile
#endif

static FATFS fs_volume;
static bool fs_registered;
static bool fs_mounting;
static bool fs_write_allowed;

static bool fs_thread_context(void) {
  return (__get_IPSR() == 0U) && (__get_PRIMASK() == 0U) &&
         (__get_BASEPRI() == 0U) && (__get_FAULTMASK() == 0U);
}

static FRESULT fs_result(wio_sd_result_t result) {
  switch (result) {
    case WIO_SD_OK:              return FR_OK;
    case WIO_SD_PARAMETER:       return FR_INVALID_PARAMETER;
    case WIO_SD_NOT_READY:       return FR_NOT_READY;
    case WIO_SD_WRITE_PROTECTED: return FR_WRITE_PROTECTED;
    case WIO_SD_TIMEOUT:         return FR_TIMEOUT;
    case WIO_SD_UNSUPPORTED:     return FR_NO_FILESYSTEM;
    case WIO_SD_BUSY:            return FR_LOCKED;
    default:                    return FR_DISK_ERR;
  }
}

static DRESULT disk_result(wio_sd_result_t result) {
  switch (result) {
    case WIO_SD_OK:              return RES_OK;
    case WIO_SD_PARAMETER:       return RES_PARERR;
    case WIO_SD_WRITE_PROTECTED: return RES_WRPRT;
    case WIO_SD_NOT_READY:       return RES_NOTRDY;
    case WIO_SD_BUSY:            return RES_NOTRDY;
    default:                    return RES_ERROR;
  }
}

static bool fs_geometry_valid(uint64_t card_sectors) {
  /* FatFs validates the BPB's internal layout, but FAT12/16/32 mount does not
   * compare that layout to the physical medium's sector count. Use widened
   * arithmetic before permitting file access, especially before opening the
   * write gate: a wrapped 32-bit sector could otherwise alias valid media.
   */
  if ((fs_volume.fs_type < FS_FAT12) || (fs_volume.fs_type > FS_FAT32) ||
      (fs_volume.n_fats < 1U) || (fs_volume.n_fats > 2U) ||
      (fs_volume.fsize == 0U) || (fs_volume.csize == 0U) ||
      ((fs_volume.csize & (fs_volume.csize - 1U)) != 0U) ||
      (fs_volume.n_fatent < 3U)) {
    return false;
  }
  const uint64_t fat_start = fs_volume.fatbase;
  const uint64_t fat_end = fat_start + (uint64_t)fs_volume.fsize * fs_volume.n_fats;
  const uint64_t data_start = fs_volume.database;
  const uint64_t data_end = data_start +
                            (uint64_t)(fs_volume.n_fatent - 2U) * fs_volume.csize;
  if ((fs_volume.volbase >= fat_start) || (fat_end > data_start) ||
      (data_end > card_sectors)) {
    return false;
  }
  if (fs_volume.fs_type == FS_FAT32) {
    return (data_start == fat_end) && (fs_volume.n_rootdir == 0U) &&
           (fs_volume.dirbase >= 2U) && (fs_volume.dirbase < fs_volume.n_fatent);
  }
  const uint64_t root_sectors = (uint64_t)fs_volume.n_rootdir * 32U / WIO_SD_SECTOR_SIZE;
  return (fs_volume.n_rootdir != 0U) && ((fs_volume.n_rootdir % 16U) == 0U) &&
         (fs_volume.dirbase == fat_end) && (data_start == fat_end + root_sectors);
}

DSTATUS disk_status(BYTE drive) {
  wio_sd_info_t info;
  if ((drive != 0U) || (!fs_registered && !fs_mounting) ||
      (wio_sd_get_info(&info) != WIO_SD_OK)) {
    return STA_NOINIT | STA_PROTECT;
  }
  return (!fs_write_allowed || !info.writable || info.write_protected) ?
          STA_PROTECT : 0U;
}

DSTATUS disk_initialize(BYTE drive) {
  /* Only the wrapper initializes hardware. A later FatFs access after an I/O
   * fault must not reset/re-mount the card and clear locks on dirty objects.
   */
  return disk_status(drive);
}

DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count) {
  if ((drive != 0U) || (buffer == NULL) || (count == 0U)) {
    return RES_PARERR;
  }
  if ((disk_status(drive) & STA_NOINIT) != 0U) {
    return RES_NOTRDY;
  }
  return disk_result(wio_sd_read_blocks(sector, buffer, count));
}

DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector, UINT count) {
  if ((drive != 0U) || (buffer == NULL) || (count == 0U)) {
    return RES_PARERR;
  }
  const DSTATUS status = disk_status(drive);
  if ((status & STA_NOINIT) != 0U) {
    return RES_NOTRDY;
  }
  if ((status & STA_PROTECT) != 0U) {
    return RES_WRPRT;
  }
  return disk_result(wio_sd_write_blocks(sector, buffer, count));
}

DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer) {
  if (drive != 0U) {
    return RES_PARERR;
  }
  if ((disk_status(drive) & STA_NOINIT) != 0U) {
    return RES_NOTRDY;
  }
  if (command == CTRL_SYNC) {
    return disk_result(wio_sd_sync());
  }
  if (buffer == NULL) {
    return RES_PARERR;
  }
  if (command == GET_SECTOR_SIZE) {
    *(WORD *)buffer = WIO_SD_SECTOR_SIZE;
    return RES_OK;
  }
  if (command == GET_SECTOR_COUNT) {
    wio_sd_info_t info;
    if (wio_sd_get_info(&info) != WIO_SD_OK) {
      return RES_NOTRDY;
    }
    if (info.sector_count > UINT32_MAX) {
      return RES_PARERR;
    }
    *(LBA_t *)buffer = (LBA_t)info.sector_count;
    return RES_OK;
  }
  /* No format/erase geometry guessed; GET_BLOCK_SIZE and CTRL_TRIM unsupported. */
  return RES_PARERR;
}

FRESULT wio_fs_mount(bool writable) {
  if (!fs_thread_context() || fs_mounting) {
    return FR_LOCKED;
  }
  if (fs_registered) {
    const FRESULT previous = wio_fs_unmount();
    if (previous != FR_OK) {
      return previous; /* FR_LOCKED preserves the previous mount and write gate. */
    }
  }
  fs_write_allowed = false;
  (void)wio_sd_set_writable(false);
  FRESULT result = fs_result(wio_sd_init());
  if (result != FR_OK) {
    return result;
  }
  wio_sd_info_t info;
  result = fs_result(wio_sd_get_info(&info));
  if ((result == FR_OK) && ((info.sector_size != WIO_SD_SECTOR_SIZE) ||
                            (info.sector_count == 0U) ||
                            (info.sector_count > UINT32_MAX))) {
    result = FR_NO_FILESYSTEM; /* This small FAT profile has 32-bit LBAs. */
  }
  if ((result == FR_OK) && writable && info.write_protected) {
    result = FR_WRITE_PROTECTED;
  }
  if (result == FR_OK) {
    memset(&fs_volume, 0, sizeof(fs_volume));
    fs_mounting = true;
    /* Mount inspection is always read-only, even when the caller opted in. */
    result = f_mount(&fs_volume, "0:", 1U);
    fs_mounting = false;
  }
  if (result == FR_OK) {
    /* Upstream may ignore an optional FAT32 FSInfo read failure. The SD layer
     * invalidates readiness after any transfer/CRC failure, so do not commit
     * a seemingly successful read-only mount with a dead transport.
     */
    result = fs_result(wio_sd_get_info(&info));
  }
  if ((result == FR_OK) && !fs_geometry_valid(info.sector_count)) {
    result = FR_NO_FILESYSTEM;
  }
  if ((result == FR_OK) && writable) {
    result = fs_result(wio_sd_set_writable(true));
  }
  if (result != FR_OK) {
    (void)f_mount(NULL, "0:", 0U);
    (void)wio_sd_set_writable(false);
    (void)wio_sd_deinit();
    fs_write_allowed = false;
    fs_registered = false;
    return result;
  }
  fs_write_allowed = writable;
  fs_registered = true;
  return FR_OK;
}

FRESULT wio_fs_unmount(void) {
  if (!fs_thread_context() || fs_mounting) {
    return FR_LOCKED;
  }
  if (fs_registered) {
    if (wio_fatfs_open_objects(&fs_volume) != 0U) {
      return FR_LOCKED;
    }
    if (fs_volume.wflag != 0U) {
      return FR_DISK_ERR; /* Do not discard metadata left dirty by an I/O error. */
    }
    const FRESULT synced = fs_result(wio_sd_sync());
    if (synced != FR_OK) {
      return synced;
    }
    const FRESULT unmounted = f_mount(NULL, "0:", 0U);
    if (unmounted != FR_OK) {
      return unmounted;
    }
  }
  fs_registered = false;
  fs_write_allowed = false;
  (void)wio_sd_set_writable(false);
  return fs_result(wio_sd_deinit());
}

bool wio_fs_is_mounted(void) {
  /* Ownership persists after a media error so dirty handles are not forgotten. */
  return fs_registered;
}

bool wio_fs_is_writable(void) {
  wio_sd_info_t info;
  return fs_registered && fs_write_allowed &&
         (wio_sd_get_info(&info) == WIO_SD_OK) && info.writable &&
         !info.write_protected;
}
