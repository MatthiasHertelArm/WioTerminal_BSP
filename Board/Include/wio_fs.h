/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WIO_FS_H_
#define WIO_FS_H_

#include <stdbool.h>
#include "ff.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One FAT12/16/32 microSD volume, "0:". Use mount(false) for read-only access;
 * only mount(true) enables writes. Formatting, trim, exFAT and long filenames
 * are disabled. Existing long names can be accessed by their 8.3 aliases.
 * Standard FatFs f_open/f_read/f_readdir/f_write/etc. operate on this volume.
 * New/modified timestamps use the configured fixed date, not a real-time clock.
 *
 * Call wio_sdk_init() first. All calls, including the standard FatFs calls,
 * must be serialized in thread mode with interrupts enabled. Use ONLY these
 * mount/unmount functions, never f_mount/f_unmount. Do not call raw wio_sd or
 * Driver_SPI6 while mounted; removing a card while mounted is unsupported.
 *
 * Close all files/directories before unmounting. Open files and non-root
 * directories cause FR_LOCKED without changing the current write gate/mount.
 * Upstream does not track root-directory handles: close those explicitly.
 * Dirty metadata or a failed media sync prevent unmounting instead of silently
 * dropping data. After media failure, no automatic reinitialization/remount
 * occurs; preserve the error and recover/reboot explicitly if close cannot
 * succeed. Neither API formats, erases, or repairs a card.
 */
FRESULT wio_fs_mount(bool writable);
FRESULT wio_fs_unmount(void);
bool wio_fs_is_mounted(void);
bool wio_fs_is_writable(void);

#ifdef __cplusplus
}
#endif
#endif
