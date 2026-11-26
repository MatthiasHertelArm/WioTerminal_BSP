/* SPDX-License-Identifier: Apache-2.0 */
/* Runs the real FatFs and Wio bridge against RAM only. No host/board disk I/O. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wio_fs.h"
#include "wio_sd.h"
#include "diskio.h"

#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)
#define RAM_SECTORS 256U

uint32_t storage_test_ipsr;
uint32_t storage_test_primask;
uint32_t storage_test_basepri;
uint32_t storage_test_faultmask;

static uint8_t ram_disk[RAM_SECTORS][WIO_SD_SECTOR_SIZE];
static bool ready, writable, protected_card, fail_init, fail_enable, fail_write;
static int32_t fail_read_sector = -1;
static uint64_t geometry = RAM_SECTORS;
static uint32_t init_count, read_count, write_count, sync_count;
static const char fixture[] = "Wio storage fixture\n";

wio_sd_result_t wio_sd_init(void) {
  ++init_count;
  writable = false;
  ready = !fail_init;
  return ready ? WIO_SD_OK : WIO_SD_NOT_READY;
}
wio_sd_result_t wio_sd_deinit(void) {
  ready = false;
  writable = false;
  return WIO_SD_OK;
}
wio_sd_result_t wio_sd_get_info(wio_sd_info_t *info) {
  if (!ready) return WIO_SD_NOT_READY;
  if (info == NULL) return WIO_SD_PARAMETER;
  memset(info, 0, sizeof(*info));
  info->sector_count = geometry;
  info->sector_size = WIO_SD_SECTOR_SIZE;
  info->write_protected = protected_card;
  info->writable = writable;
  return WIO_SD_OK;
}
wio_sd_result_t wio_sd_set_writable(bool value) {
  if (!value) {
    writable = false;
    return WIO_SD_OK;
  }
  if (!ready) return WIO_SD_NOT_READY;
  if (protected_card) return WIO_SD_WRITE_PROTECTED;
  if (fail_enable) return WIO_SD_IO;
  writable = true;
  return WIO_SD_OK;
}
wio_sd_result_t wio_sd_read_blocks(uint32_t sector, void *buffer, uint32_t count) {
  if (!ready) return WIO_SD_NOT_READY;
  if (!buffer || !count || (uint64_t)sector + count > RAM_SECTORS) return WIO_SD_PARAMETER;
  ++read_count;
  if (fail_read_sector >= 0 && sector == (uint32_t)fail_read_sector) {
    ready = false;
    writable = false;
    return WIO_SD_IO;
  }
  memcpy(buffer, ram_disk[sector], (size_t)count * WIO_SD_SECTOR_SIZE);
  return WIO_SD_OK;
}
wio_sd_result_t wio_sd_write_blocks(uint32_t sector, const void *buffer, uint32_t count) {
  if (!ready) return WIO_SD_NOT_READY;
  if (!writable || protected_card) return WIO_SD_WRITE_PROTECTED;
  if (!buffer || !count || (uint64_t)sector + count > RAM_SECTORS) return WIO_SD_PARAMETER;
  ++write_count;
  if (fail_write) {
    ready = false;
    writable = false;
    return WIO_SD_IO;
  }
  memcpy(ram_disk[sector], buffer, (size_t)count * WIO_SD_SECTOR_SIZE);
  return WIO_SD_OK;
}
wio_sd_result_t wio_sd_sync(void) {
  ++sync_count;
  return ready ? WIO_SD_OK : WIO_SD_NOT_READY;
}

static void put16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}
static void put32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4U; ++i) p[i] = (uint8_t)(v >> (8U * i));
}
static void fat12(uint32_t cluster, uint16_t value) {
  const uint32_t offset = cluster + cluster / 2U;
  for (uint32_t copy = 1U; copy <= 2U; ++copy) {
    uint8_t *p = &ram_disk[copy][offset];
    uint16_t pair = (uint16_t)(p[0] | (uint16_t)p[1] << 8);
    pair = (cluster & 1U) ? (uint16_t)((pair & 0x000FU) | (value << 4)) :
                            (uint16_t)((pair & 0xF000U) | value);
    put16(p, pair);
  }
}
static void entry(uint8_t *p, const char name[11], uint8_t attr,
                  uint16_t cluster, uint32_t size) {
  memcpy(p, name, 11U); p[11] = attr;
  put16(p + 26, cluster); put32(p + 28, size);
}

/* Test fixture construction, NOT a production formatter. Two FAT12 tables,
 * 32 root entries, a regular file and one subdirectory containing a file.
 */
static void make_fixture(void) {
  CHECK(!wio_fs_is_mounted());
  memset(ram_disk, 0, sizeof(ram_disk));
  ready = writable = protected_card = fail_init = fail_enable = fail_write = false;
  fail_read_sector = -1;
  geometry = RAM_SECTORS;
  init_count = read_count = write_count = sync_count = 0U;
  uint8_t *b = ram_disk[0];
  b[0] = 0xEBU; b[1] = 0x3CU; b[2] = 0x90U;
  memcpy(b + 3, "MSDOS5.0", 8U);
  put16(b + 11, 512U); b[13] = 1U; put16(b + 14, 1U);
  b[16] = 2U; put16(b + 17, 32U); put16(b + 19, RAM_SECTORS);
  b[21] = 0xF8U; put16(b + 22, 1U); put16(b + 24, 1U); put16(b + 26, 1U);
  b[38] = 0x29U; memcpy(b + 54, "FAT12   ", 8U);
  b[510] = 0x55U; b[511] = 0xAAU;
  fat12(0U, 0xFF8U); fat12(1U, 0xFFFU);
  fat12(2U, 0xFFFU); fat12(3U, 0xFFFU); fat12(4U, 0xFFFU);
  entry(ram_disk[3], "WIOTEST TXT", AM_ARC, 2U, sizeof(fixture) - 1U);
  entry(ram_disk[3] + 32U, "SUBDIR     ", AM_DIR, 3U, 0U);
  memcpy(ram_disk[5], fixture, sizeof(fixture) - 1U);
  entry(ram_disk[6], ".          ", AM_DIR, 3U, 0U);
  entry(ram_disk[6] + 32U, "..         ", AM_DIR, 0U, 0U);
  entry(ram_disk[6] + 64U, "NESTED  TXT", AM_ARC, 4U, 6U);
  memcpy(ram_disk[7], "nested", 6U);
}

static void test_read_only(void) {
  make_fixture();
  CHECK(wio_fs_mount(false) == FR_OK);
  CHECK(wio_fs_is_mounted() && !wio_fs_is_writable() && !writable);
  CHECK(disk_status(0U) == STA_PROTECT);
  FIL file, second;
  UINT transferred;
  char data[64] = {0};
  CHECK(f_open(&file, "0:/WIOTEST.TXT", FA_READ) == FR_OK);
  CHECK(f_open(&second, "0:/WIOTEST.TXT", FA_READ) == FR_OK);
  CHECK(wio_fatfs_open_objects(file.obj.fs) == 1U);
  CHECK(f_read(&file, data, sizeof(data), &transferred) == FR_OK);
  CHECK(transferred == sizeof(fixture) - 1U && memcmp(data, fixture, transferred) == 0);
  CHECK(wio_fs_unmount() == FR_LOCKED);
  CHECK(wio_fs_mount(true) == FR_LOCKED && !writable && init_count == 1U);
  CHECK(f_close(&file) == FR_OK);
  CHECK(wio_fs_unmount() == FR_LOCKED);
  CHECK(f_close(&second) == FR_OK);

  DIR directory;
  FILINFO found;
  CHECK(f_opendir(&directory, "0:/") == FR_OK);
  CHECK(f_readdir(&directory, &found) == FR_OK && strcmp(found.fname, "WIOTEST.TXT") == 0);
  CHECK(f_readdir(&directory, &found) == FR_OK && strcmp(found.fname, "SUBDIR") == 0);
  CHECK(f_readdir(&directory, &found) == FR_OK && found.fname[0] == '\0');
  CHECK(f_closedir(&directory) == FR_OK);
  CHECK(f_opendir(&directory, "0:/SUBDIR") == FR_OK);
  CHECK(wio_fs_unmount() == FR_LOCKED);
  CHECK(f_readdir(&directory, &found) == FR_OK && strcmp(found.fname, "NESTED.TXT") == 0);
  CHECK(f_closedir(&directory) == FR_OK);
  CHECK(f_open(&file, "0:/WIOTEST.TXT", FA_WRITE) == FR_WRITE_PROTECTED);
  CHECK(f_unlink("0:/WIOTEST.TXT") == FR_WRITE_PROTECTED);
  CHECK(f_mkdir("0:/NOPE") == FR_WRITE_PROTECTED);
  CHECK(disk_write(0U, (const BYTE *)data, 0U, 1U) == RES_WRPRT);
  CHECK(disk_write(1U, (const BYTE *)data, 0U, 1U) == RES_PARERR);
  CHECK(disk_read(0U, NULL, 0U, 1U) == RES_PARERR);
  LBA_t sectors = 0U;
  WORD sector_size = 0U;
  CHECK(disk_ioctl(0U, GET_SECTOR_COUNT, &sectors) == RES_OK && sectors == RAM_SECTORS);
  CHECK(disk_ioctl(0U, GET_SECTOR_SIZE, &sector_size) == RES_OK && sector_size == 512U);
  CHECK(disk_ioctl(0U, CTRL_TRIM, &sectors) == RES_PARERR);
  CHECK(disk_ioctl(0U, GET_BLOCK_SIZE, &sectors) == RES_PARERR);
  CHECK(disk_ioctl(0U, GET_SECTOR_SIZE, NULL) == RES_PARERR);
  CHECK(write_count == 0U);
  CHECK(wio_fs_unmount() == FR_OK && !wio_fs_is_mounted() && !writable);
  CHECK((disk_status(0U) & STA_NOINIT) != 0U);
  CHECK(wio_fs_unmount() == FR_OK);
}

static void test_opt_in_and_persistence(void) {
  make_fixture();
  CHECK(wio_fs_mount(true) == FR_OK && wio_fs_is_writable());
  uint8_t data[700], readback[700];
  for (unsigned i = 0U; i < sizeof(data); ++i) data[i] = (uint8_t)(i * 7U);
  FIL file;
  UINT transferred;
  CHECK(f_open(&file, "0:/NEW.TXT", FA_CREATE_NEW | FA_WRITE) == FR_OK);
  CHECK(f_write(&file, data, sizeof(data), &transferred) == FR_OK && transferred == sizeof(data));
  CHECK(wio_fs_unmount() == FR_LOCKED && writable && wio_fs_is_writable());
  CHECK(wio_fs_mount(false) == FR_LOCKED && writable && init_count == 1U);
  CHECK(f_sync(&file) == FR_OK && write_count != 0U);
  CHECK(f_close(&file) == FR_OK);
  CHECK(wio_fs_unmount() == FR_OK && !writable);
  CHECK(wio_fs_mount(false) == FR_OK && !wio_fs_is_writable());
  CHECK(f_open(&file, "0:/NEW.TXT", FA_READ) == FR_OK);
  CHECK(f_size(&file) == sizeof(data));
  CHECK(f_read(&file, readback, sizeof(readback), &transferred) == FR_OK);
  CHECK(transferred == sizeof(data) && memcmp(data, readback, sizeof(data)) == 0);
  CHECK(f_close(&file) == FR_OK);
  CHECK(wio_fs_unmount() == FR_OK);
}

static void test_sparse_fat16(void) {
  make_fixture();
  geometry = 8192U;
  put16(ram_disk[0] + 19, 8192U);
  put16(ram_disk[0] + 22, 32U);
  memcpy(ram_disk[0] + 54, "FAT16   ", 8U);
  memset(ram_disk[1], 0, sizeof(ram_disk) - sizeof(ram_disk[0]));
  put16(ram_disk[1], 0xFFF8U); put16(ram_disk[1] + 2U, 0xFFFFU);
  put16(ram_disk[1] + 4U, 0xFFFFU);
  memcpy(ram_disk[33], ram_disk[1], WIO_SD_SECTOR_SIZE);
  entry(ram_disk[65], "WIOTEST TXT", AM_ARC, 2U, sizeof(fixture) - 1U);
  memcpy(ram_disk[67], fixture, sizeof(fixture) - 1U);
  CHECK(wio_fs_mount(false) == FR_OK);
  FIL file;
  char data[64];
  UINT transferred;
  CHECK(f_open(&file, "0:/WIOTEST.TXT", FA_READ) == FR_OK);
  CHECK(file.obj.fs->fs_type == FS_FAT16);
  CHECK(f_read(&file, data, sizeof(data), &transferred) == FR_OK);
  CHECK(transferred == sizeof(fixture) - 1U && memcmp(data, fixture, transferred) == 0);
  CHECK(f_close(&file) == FR_OK && wio_fs_unmount() == FR_OK && write_count == 0U);
}

static void test_mount_failures_and_patches(void) {
  make_fixture();
  fail_init = true;
  CHECK(wio_fs_mount(true) == FR_NOT_READY && !writable && !wio_fs_is_mounted());
  fail_init = false; protected_card = true;
  CHECK(wio_fs_mount(true) == FR_WRITE_PROTECTED && !writable && !wio_fs_is_mounted());
  protected_card = false; fail_enable = true;
  CHECK(wio_fs_mount(true) == FR_DISK_ERR && !writable && !wio_fs_is_mounted());
  fail_enable = false; geometry = UINT64_C(1) << 32;
  CHECK(wio_fs_mount(true) == FR_NO_FILESYSTEM && !writable && !wio_fs_is_mounted());

  make_fixture();
  ram_disk[0][13] = 8U; /* Only 31 clusters: upstream patch 1 rejects this. */
  CHECK(wio_fs_mount(true) == FR_NO_FILESYSTEM && !writable);
  make_fixture();
  put16(ram_disk[0] + 19, 64U); /* Patch 1 permits a valid 64-sector FAT12 volume. */
  CHECK(wio_fs_mount(false) == FR_OK);
  CHECK(wio_fs_unmount() == FR_OK);

  make_fixture();
  /* Internally plausible FAT12 BPB, but its data extent exceeds the card. */
  put16(ram_disk[0] + 19, 32768U);
  ram_disk[0][13] = 128U;
  CHECK(wio_fs_mount(false) == FR_NO_FILESYSTEM && !writable && !wio_fs_is_mounted());
  CHECK(wio_fs_mount(true) == FR_NO_FILESYSTEM && !writable && !wio_fs_is_mounted());
  CHECK(write_count == 0U);

  make_fixture();
  uint8_t *b = ram_disk[0];
  put16(b + 17, 0U); put16(b + 19, 0U); put16(b + 22, 0U);
  put32(b + 32, 0x300000U); put32(b + 36, 0x200000U); put32(b + 44, 2U);
  memcpy(b + 82, "FAT32   ", 8U);
  CHECK(wio_fs_mount(true) == FR_NO_FILESYSTEM && !writable); /* Patch 2 overflow guard. */
  CHECK(write_count == 0U);

  make_fixture();
  /* Sparse FAT32 fixture: mount needs only boot/FSInfo sectors. Upstream allows
   * FSInfo read failure; the wrapper must notice the SD transport is now dead.
   */
  b = ram_disk[0];
  geometry = 70000U;
  put16(b + 14, 32U); put16(b + 17, 0U); put16(b + 19, 0U); put16(b + 22, 0U);
  put32(b + 32, 70000U); put32(b + 36, 550U); put16(b + 40, 0U); put16(b + 42, 0U);
  put32(b + 44, 2U); put16(b + 48, 1U); memcpy(b + 82, "FAT32   ", 8U);
  CHECK(wio_fs_mount(false) == FR_OK && wio_fs_unmount() == FR_OK);
  read_count = 0U;
  fail_read_sector = 1;
  CHECK(wio_fs_mount(false) == FR_NOT_READY && !wio_fs_is_mounted() && !writable);
  CHECK(read_count == 2U && write_count == 0U);
}

static void test_context_and_dirty_window(void) {
  make_fixture();
  storage_test_ipsr = 1U;
  CHECK(wio_fs_mount(false) == FR_LOCKED && init_count == 0U);
  storage_test_ipsr = 0U;
  CHECK(wio_fs_mount(true) == FR_OK);
  storage_test_primask = 1U;
  CHECK(wio_fs_unmount() == FR_LOCKED && writable);
  storage_test_primask = 0U;
  FIL file;
  CHECK(f_open(&file, "0:/WIOTEST.TXT", FA_READ) == FR_OK);
  FATFS *volume = file.obj.fs;
  CHECK(f_close(&file) == FR_OK);
  volume->wflag = 1U; /* White-box simulation of unsaved metadata after an error. */
  CHECK(wio_fs_unmount() == FR_DISK_ERR && writable && wio_fs_is_mounted());
  volume->wflag = 0U;
  CHECK(wio_fs_unmount() == FR_OK);
}

static void test_media_failure_preserves_open_file(void) {
  make_fixture();
  CHECK(wio_fs_mount(true) == FR_OK);
  FIL file;
  UINT written;
  uint8_t data[512] = {0};
  CHECK(f_open(&file, "0:/WIOTEST.TXT", FA_WRITE) == FR_OK);
  fail_write = true;
  CHECK(f_write(&file, data, sizeof(data), &written) == FR_DISK_ERR);
  CHECK(!ready && !writable && !wio_fs_is_writable() && wio_fs_is_mounted());
  const uint32_t initializations = init_count;
  CHECK((disk_initialize(0U) & STA_NOINIT) != 0U);
  CHECK(init_count == initializations); /* No automatic remount invalidates locks. */
  CHECK(f_close(&file) != FR_OK);
  CHECK(wio_fs_unmount() == FR_LOCKED && wio_fs_is_mounted());
  CHECK(wio_fs_mount(false) == FR_LOCKED && init_count == initializations);
  /* Deliberately leave this failed dirty handle locked until process teardown. */
}

int main(void) {
  CHECK(FF_FS_LOCK > 0 && FF_USE_MKFS == 0 && FF_USE_TRIM == 0);
  CHECK(FF_FS_EXFAT == 0 && FF_USE_LFN == 0);
  test_read_only();
  test_opt_in_and_persistence();
  test_sparse_fat16();
  test_mount_failures_and_patches();
  test_context_and_dirty_window();
  test_media_failure_preserves_open_file();
  puts("storage_fs: real FatFs/RAM-disk safety and persistence tests passed");
  return 0;
}
