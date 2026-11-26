/* SPDX-License-Identifier: Apache-2.0 */
/*
 * SD SPI sequence reference: ChaN, FatFs ffsample.zip, generic/sdmm.c (2019).
 * Copyright (C) 2019, ChaN, all right reserved.
 * This software is a free software and there is NO WARRANTY. No restriction
 * on use. You can use, modify and redistribute it for personal, non-profit or
 * commercial products UNDER YOUR RESPONSIBILITY. Redistributions of source
 * code must retain the above copyright notice.
 *
 * CRC7 algorithm and CRC/protocol cross-check: SdFat 2.3.1,
 * cda057318bec196183d4cc92b01bc1dd64bbfb02, SdSpiCard.cpp / SdCardInfo.h.
 * Copyright (c) 2011-2025 Bill Greiman. MIT License:
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
#include "wio_sd.h"

#include <stddef.h>
#include <string.h>

#include "Driver_SPI.h"
#include "wio_sdk.h"
#include "wio_terminal.h"

extern ARM_DRIVER_SPI Driver_SPI6;

#define SD_INIT_HZ             400000U
#define SD_DATA_HZ            4000000U
#define SD_SPI_TIMEOUT_MS        100U
#define SD_SPI_SPIN_LIMIT    2000000U
#define SD_INIT_TIMEOUT_MS      2000U
#define SD_INIT_ATTEMPTS        2000U
#define SD_READY_TIMEOUT_MS      600U
#define SD_DATA_TIMEOUT_MS       300U
#define SD_POLL_ATTEMPTS       10000U

#define SD_CMD0    0U
#define SD_CMD8    8U
#define SD_CMD9    9U
#define SD_CMD13  13U
#define SD_CMD16  16U
#define SD_CMD17  17U
#define SD_CMD24  24U
#define SD_CMD55  55U
#define SD_CMD58  58U
#define SD_CMD59  59U
#define SD_ACMD41 41U

static bool sd_initialized;
static bool sd_ready;
static bool sd_busy;
static bool sd_transfer_failed;
static wio_sd_info_t sd_info;

/* The IRQ-driven driver never retains an automatic or caller-owned buffer.
 * A separate sector buffer survives the subsequent two-byte CRC transaction.
 */
static uint8_t sd_wire[WIO_SD_SECTOR_SIZE];
static uint8_t sd_sector[WIO_SD_SECTOR_SIZE];

static bool sd_thread_context(void) {
  return (__get_IPSR() == 0U) && (__get_PRIMASK() == 0U) &&
         (__get_BASEPRI() == 0U) && (__get_FAULTMASK() == 0U);
}

static bool sd_claim(void) {
  if (!sd_thread_context() || sd_busy) {
    return false;
  }
  sd_busy = true;
  return true;
}

static uint8_t sd_crc7(const uint8_t *bytes, uint32_t count) {
  uint8_t crc = 0U;
  for (uint32_t i = 0U; i < count; ++i) {
    uint8_t value = bytes[i];
    for (uint32_t bit = 0U; bit < 8U; ++bit) {
      crc = (uint8_t)(crc << 1);
      if (((value ^ crc) & 0x80U) != 0U) {
        crc ^= 0x09U;
      }
      value = (uint8_t)(value << 1);
    }
  }
  return (uint8_t)((crc << 1) | 1U);
}

static uint16_t sd_crc16(const uint8_t *bytes, uint32_t count) {
  uint16_t crc = 0U;
  for (uint32_t i = 0U; i < count; ++i) {
    crc ^= (uint16_t)((uint16_t)bytes[i] << 8);
    for (uint32_t bit = 0U; bit < 8U; ++bit) {
      crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000U) ? 0x1021U : 0U));
    }
  }
  return crc;
}

static wio_sd_result_t sd_shutdown(void) {
  sd_ready = false;
  sd_info.writable = false;
  if (!sd_initialized) {
    return WIO_SD_OK;
  }
  wio_gpio_write(WIO_PIN_SD_CS, 1U);
  (void)Driver_SPI6.Control(ARM_SPI_ABORT_TRANSFER, 0U);
  const int32_t powered_off = Driver_SPI6.PowerControl(ARM_POWER_OFF);
  const int32_t uninitialized = Driver_SPI6.Uninitialize();
  sd_initialized = false;
  /* The Wio driver disables all SERCOM IRQs and releases its buffers even if
   * peripheral synchronization fails. Keep static buffers for extra safety.
   */
  return ((powered_off == ARM_DRIVER_OK) &&
          (uninitialized == ARM_DRIVER_OK)) ? WIO_SD_OK : WIO_SD_IO;
}

static wio_sd_result_t sd_finish(wio_sd_result_t result) {
  if (result != WIO_SD_OK) {
    (void)sd_shutdown();
  }
  sd_busy = false;
  return result;
}

static wio_sd_result_t sd_wait_transfer(uint32_t count) {
  const uint32_t started = wio_millis();
  wio_sd_result_t result = WIO_SD_TIMEOUT;
  for (uint32_t spin = 0U; spin < SD_SPI_SPIN_LIMIT; ++spin) {
    const ARM_SPI_STATUS status = Driver_SPI6.GetStatus();
    if ((status.data_lost != 0U) || (status.mode_fault != 0U)) {
      result = WIO_SD_IO;
      break;
    }
    if (status.busy == 0U) {
      if (Driver_SPI6.GetDataCount() == count) {
        return WIO_SD_OK;
      }
      result = WIO_SD_IO;
      break;
    }
    if ((uint32_t)(wio_millis() - started) >= SD_SPI_TIMEOUT_MS) {
      break;
    }
  }
  sd_transfer_failed = true;
  return result;
}

static wio_sd_result_t sd_send(const uint8_t *bytes, uint32_t count) {
  if (sd_transfer_failed || (Driver_SPI6.GetStatus().busy != 0U)) {
    return WIO_SD_IO;
  }
  memcpy(sd_wire, bytes, count);
  if (Driver_SPI6.Send(sd_wire, count) != ARM_DRIVER_OK) {
    sd_transfer_failed = true;
    return WIO_SD_IO;
  }
  return sd_wait_transfer(count);
}

static wio_sd_result_t sd_receive(uint8_t *bytes, uint32_t count) {
  if (sd_transfer_failed || (Driver_SPI6.GetStatus().busy != 0U)) {
    return WIO_SD_IO;
  }
  if (Driver_SPI6.Receive(sd_wire, count) != ARM_DRIVER_OK) {
    sd_transfer_failed = true;
    return WIO_SD_IO;
  }
  const wio_sd_result_t result = sd_wait_transfer(count);
  if (result == WIO_SD_OK) {
    memcpy(bytes, sd_wire, count);
  }
  return result;
}

static wio_sd_result_t sd_deselect(void) {
  uint8_t ignored;
  wio_gpio_write(WIO_PIN_SD_CS, 1U);
  return sd_receive(&ignored, 1U); /* Eight clocks release MISO. */
}

static wio_sd_result_t sd_wait_ready(void) {
  const uint32_t started = wio_millis();
  for (uint32_t poll = 0U; poll < SD_POLL_ATTEMPTS; ++poll) {
    uint8_t value;
    const wio_sd_result_t result = sd_receive(&value, 1U);
    if (result != WIO_SD_OK) {
      return result;
    }
    if (value == 0xFFU) {
      return WIO_SD_OK;
    }
    if ((uint32_t)(wio_millis() - started) >= SD_READY_TIMEOUT_MS) {
      break;
    }
    wio_delay_us(100U);
  }
  /* Do not re-enter the outer CMD0 retry loop when the card never releases
   * busy. This also bounds initialization when the millisecond clock stops.
   */
  sd_transfer_failed = true;
  return WIO_SD_TIMEOUT;
}

static wio_sd_result_t sd_select(void) {
  uint8_t ignored;
  wio_gpio_write(WIO_PIN_SD_CS, 0U);
  /* The first byte after CS falls enables the card's DO/busy output. Do not
   * mistake a previous high-impedance MISO value for an already-ready card.
   */
  const wio_sd_result_t result = sd_receive(&ignored, 1U);
  return (result == WIO_SD_OK) ? sd_wait_ready() : result;
}

/* Every command is a fresh single-block transaction; no CMD12/CMD25 state. */
static wio_sd_result_t sd_command(uint8_t command, uint32_t argument,
                                  uint8_t *response) {
  uint8_t frame[6] = {
    (uint8_t)(0x40U | command), (uint8_t)(argument >> 24),
    (uint8_t)(argument >> 16), (uint8_t)(argument >> 8),
    (uint8_t)argument, 0U
  };
  wio_sd_result_t result = sd_deselect();
  if (result != WIO_SD_OK) {
    return result;
  }
  result = sd_select();
  if (result != WIO_SD_OK) {
    return result;
  }
  frame[5] = sd_crc7(frame, 5U);
  result = sd_send(frame, sizeof(frame));
  if (result != WIO_SD_OK) {
    return result;
  }
  for (uint32_t poll = 0U; poll < 10U; ++poll) {
    result = sd_receive(response, 1U);
    if (result != WIO_SD_OK) {
      return result;
    }
    if ((*response & 0x80U) == 0U) {
      return ((*response & 0x08U) != 0U) ? WIO_SD_CRC : WIO_SD_OK;
    }
  }
  return WIO_SD_TIMEOUT;
}

static wio_sd_result_t sd_command_ready(uint8_t command, uint32_t argument) {
  uint8_t response;
  const wio_sd_result_t result = sd_command(command, argument, &response);
  if (result != WIO_SD_OK) {
    return result;
  }
  return (response == 0U) ? WIO_SD_OK : WIO_SD_IO;
}

static wio_sd_result_t sd_read_packet(uint32_t count) {
  const uint32_t started = wio_millis();
  uint8_t token = 0xFFU;
  for (uint32_t poll = 0U; poll < SD_POLL_ATTEMPTS; ++poll) {
    const wio_sd_result_t result = sd_receive(&token, 1U);
    if (result != WIO_SD_OK) {
      return result;
    }
    if (token != 0xFFU) {
      break;
    }
    if ((uint32_t)(wio_millis() - started) >= SD_DATA_TIMEOUT_MS) {
      break;
    }
    wio_delay_us(100U);
  }
  if (token == 0xFFU) {
    return WIO_SD_TIMEOUT;
  }
  if (token != 0xFEU) {
    return WIO_SD_IO; /* Card error token, not a data packet. */
  }
  wio_sd_result_t result = sd_receive(sd_sector, count);
  if (result != WIO_SD_OK) {
    return result;
  }
  uint8_t trailer[2];
  result = sd_receive(trailer, sizeof(trailer));
  if (result != WIO_SD_OK) {
    return result;
  }
  const uint16_t received = (uint16_t)(((uint16_t)trailer[0] << 8) | trailer[1]);
  return (received == sd_crc16(sd_sector, count)) ? WIO_SD_OK : WIO_SD_CRC;
}

static wio_sd_result_t sd_parse_csd(void) {
  const uint8_t *csd = sd_info.csd;
  const uint32_t structure = csd[0] >> 6;
  if (structure == 0U) {
    const uint32_t read_length = csd[5] & 15U;
    const uint32_t size = ((uint32_t)(csd[6] & 3U) << 10) |
                          ((uint32_t)csd[7] << 2) | (csd[8] >> 6);
    const uint32_t mult = ((uint32_t)(csd[9] & 3U) << 1) | (csd[10] >> 7);
    if (sd_info.block_addressed || (read_length < 9U) || (read_length > 11U)) {
      return WIO_SD_UNSUPPORTED;
    }
    sd_info.sector_count = ((uint64_t)size + 1U) << (mult + read_length - 7U);
    /* SDSC byte arguments must fit all the way to the final 512-byte sector. */
    if (sd_info.sector_count > (UINT64_C(1) << 23)) {
      return WIO_SD_UNSUPPORTED;
    }
  } else if (structure == 1U) {
    const uint32_t size = ((uint32_t)(csd[7] & 63U) << 16) |
                          ((uint32_t)csd[8] << 8) | csd[9];
    if (!sd_info.block_addressed || ((csd[5] & 15U) != 9U)) {
      return WIO_SD_UNSUPPORTED;
    }
    sd_info.sector_count = ((uint64_t)size + 1U) << 10;
  } else {
    return WIO_SD_UNSUPPORTED;
  }
  sd_info.sector_size = WIO_SD_SECTOR_SIZE;
  sd_info.write_protected = (csd[14] & 0x30U) != 0U;
  return (sd_info.sector_count != 0U) ? WIO_SD_OK : WIO_SD_UNSUPPORTED;
}

wio_sd_result_t wio_sd_init(void) {
  if (!sd_claim()) {
    return WIO_SD_BUSY;
  }
  sd_info.writable = false;
  sd_ready = false;
  wio_sd_result_t result = sd_shutdown();
  if (result != WIO_SD_OK) {
    return sd_finish(result);
  }
  if (Driver_SPI6.GetStatus().busy != 0U) {
    sd_busy = false;
    return WIO_SD_BUSY;
  }
  memset(&sd_info, 0, sizeof(sd_info));
  sd_transfer_failed = false;
  wio_gpio_output(WIO_PIN_SD_CS, 1U);
  if (Driver_SPI6.Initialize(NULL) != ARM_DRIVER_OK) {
    return sd_finish(WIO_SD_IO);
  }
  sd_initialized = true;
  if ((Driver_SPI6.PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK) ||
      (Driver_SPI6.Control(ARM_SPI_MODE_MASTER | ARM_SPI_CPOL0_CPHA0 |
                           ARM_SPI_DATA_BITS(8U) | ARM_SPI_MSB_LSB |
                           ARM_SPI_SS_MASTER_UNUSED, SD_INIT_HZ) != ARM_DRIVER_OK) ||
      (Driver_SPI6.Control(ARM_SPI_SET_DEFAULT_TX_VALUE, 0xFFU) != ARM_DRIVER_OK)) {
    return sd_finish(WIO_SD_IO);
  }
  wio_gpio_output(WIO_PIN_SD_CS, 1U);
  wio_delay_ms(10U);
  uint8_t clocks[10];
  result = sd_receive(clocks, sizeof(clocks)); /* At least 74 clocks, CS high. */
  if (result != WIO_SD_OK) {
    return sd_finish(result);
  }

  uint8_t response = 0xFFU;
  const uint32_t reset_started = wio_millis();
  for (uint32_t attempt = 0U; attempt < SD_INIT_ATTEMPTS; ++attempt) {
    result = sd_command(SD_CMD0, 0U, &response);
    if ((result == WIO_SD_OK) && (response == 1U)) {
      break;
    }
    if (sd_transfer_failed ||
        (uint32_t)(wio_millis() - reset_started) >= SD_INIT_TIMEOUT_MS) {
      break;
    }
    wio_delay_ms(1U);
  }
  if ((result != WIO_SD_OK) || (response != 1U)) {
    return sd_finish((result != WIO_SD_OK) ? result : WIO_SD_TIMEOUT);
  }
  result = sd_command(SD_CMD59, 1U, &response); /* Require full CRC checking. */
  if ((result != WIO_SD_OK) || (response != 1U)) {
    return sd_finish((result != WIO_SD_OK) ? result : WIO_SD_UNSUPPORTED);
  }

  result = sd_command(SD_CMD8, 0x1AAU, &response);
  if (result != WIO_SD_OK) {
    return sd_finish(result);
  }
  const bool version2 = response == 1U;
  if (version2) {
    uint8_t r7[4];
    result = sd_receive(r7, sizeof(r7));
    if (result != WIO_SD_OK) {
      return sd_finish(result);
    }
    if ((r7[0] != 0U) || (r7[1] != 0U) || (r7[2] != 1U) || (r7[3] != 0xAAU)) {
      return sd_finish(WIO_SD_UNSUPPORTED);
    }
  } else if (response != 5U) { /* Idle + illegal command: SD v1 only. */
    return sd_finish(WIO_SD_UNSUPPORTED);
  }

  const uint32_t init_started = wio_millis();
  response = 1U;
  for (uint32_t attempt = 0U; attempt < SD_INIT_ATTEMPTS; ++attempt) {
    result = sd_command(SD_CMD55, 0U, &response);
    if ((result != WIO_SD_OK) || (response > 1U)) {
      return sd_finish((result != WIO_SD_OK) ? result : WIO_SD_UNSUPPORTED);
    }
    result = sd_command(SD_ACMD41, version2 ? 0x40000000U : 0U, &response);
    if ((result != WIO_SD_OK) || (response > 1U)) {
      return sd_finish((result != WIO_SD_OK) ? result : WIO_SD_UNSUPPORTED);
    }
    if (response == 0U) {
      break;
    }
    if ((uint32_t)(wio_millis() - init_started) >= SD_INIT_TIMEOUT_MS) {
      break;
    }
    wio_delay_ms(1U);
  }
  if (response != 0U) {
    return sd_finish(WIO_SD_TIMEOUT);
  }
  result = sd_command_ready(SD_CMD58, 0U);
  if (result != WIO_SD_OK) {
    return sd_finish(result);
  }
  uint8_t ocr_bytes[4];
  result = sd_receive(ocr_bytes, sizeof(ocr_bytes));
  if (result != WIO_SD_OK) {
    return sd_finish(result);
  }
  const uint32_t ocr = ((uint32_t)ocr_bytes[0] << 24) |
                       ((uint32_t)ocr_bytes[1] << 16) |
                       ((uint32_t)ocr_bytes[2] << 8) | ocr_bytes[3];
  if (((ocr & 0x80000000U) == 0U) || ((ocr & 0x00FF8000U) == 0U)) {
    return sd_finish(WIO_SD_UNSUPPORTED);
  }
  sd_info.block_addressed = version2 && ((ocr & 0x40000000U) != 0U);
  if (!sd_info.block_addressed) {
    result = sd_command_ready(SD_CMD16, WIO_SD_SECTOR_SIZE);
    if (result != WIO_SD_OK) {
      return sd_finish(result);
    }
  }
  result = sd_command_ready(SD_CMD9, 0U);
  if (result == WIO_SD_OK) {
    result = sd_read_packet(sizeof(sd_info.csd));
  }
  if (result != WIO_SD_OK) {
    return sd_finish(result);
  }
  memcpy(sd_info.csd, sd_sector, sizeof(sd_info.csd));
  result = sd_parse_csd();
  if (result == WIO_SD_OK) {
    result = sd_deselect();
  }
  if (result != WIO_SD_OK) {
    return sd_finish(result);
  }
  if (Driver_SPI6.Control(ARM_SPI_SET_BUS_SPEED, SD_DATA_HZ) != ARM_DRIVER_OK) {
    return sd_finish(WIO_SD_IO);
  }
  sd_ready = true;
  return sd_finish(WIO_SD_OK);
}

wio_sd_result_t wio_sd_deinit(void) {
  if (!sd_claim()) {
    return WIO_SD_BUSY;
  }
  const wio_sd_result_t result = sd_shutdown();
  sd_busy = false;
  return result;
}

wio_sd_result_t wio_sd_get_info(wio_sd_info_t *info) {
  if (info == NULL) {
    return WIO_SD_PARAMETER;
  }
  if (!sd_thread_context() || sd_busy) {
    return WIO_SD_BUSY;
  }
  if (!sd_ready) {
    return WIO_SD_NOT_READY;
  }
  *info = sd_info;
  return WIO_SD_OK;
}

wio_sd_result_t wio_sd_set_writable(bool writable) {
  if (!sd_claim()) {
    return WIO_SD_BUSY;
  }
  wio_sd_result_t result = WIO_SD_OK;
  if (!writable) {
    sd_info.writable = false;
  } else if (!sd_ready) {
    result = WIO_SD_NOT_READY;
  } else if (sd_info.write_protected) {
    result = WIO_SD_WRITE_PROTECTED;
  } else {
    sd_info.writable = true;
  }
  sd_busy = false;
  return result;
}

static wio_sd_result_t sd_validate_io(uint32_t lba, const void *buffer,
                                     uint32_t count) {
  if ((buffer == NULL) || (count == 0U) ||
      ((uint64_t)count * WIO_SD_SECTOR_SIZE > SIZE_MAX)) {
    return WIO_SD_PARAMETER;
  }
  if (!sd_ready) {
    return WIO_SD_NOT_READY;
  }
  if (((uint64_t)lba + count) > sd_info.sector_count) {
    return WIO_SD_PARAMETER;
  }
  return WIO_SD_OK;
}

wio_sd_result_t wio_sd_read_blocks(uint32_t lba, void *buffer, uint32_t count) {
  if (!sd_claim()) {
    return WIO_SD_BUSY;
  }
  wio_sd_result_t result = sd_validate_io(lba, buffer, count);
  if (result != WIO_SD_OK) {
    sd_busy = false; /* Caller errors do not tear down an otherwise good card. */
    return result;
  }
  uint8_t *out = buffer;
  for (uint32_t i = 0U; i < count; ++i) {
    const uint32_t address = sd_info.block_addressed ? lba : lba * WIO_SD_SECTOR_SIZE;
    result = sd_command_ready(SD_CMD17, address);
    if (result == WIO_SD_OK) {
      result = sd_read_packet(WIO_SD_SECTOR_SIZE);
    }
    if (result == WIO_SD_OK) {
      result = sd_deselect();
    }
    if (result != WIO_SD_OK) {
      return sd_finish(result);
    }
    memcpy(out, sd_sector, WIO_SD_SECTOR_SIZE); /* Publish only CRC-checked data. */
    out += WIO_SD_SECTOR_SIZE;
    ++lba;
  }
  return sd_finish(WIO_SD_OK);
}

wio_sd_result_t wio_sd_write_blocks(uint32_t lba, const void *buffer,
                                    uint32_t count) {
  if (!sd_claim()) {
    return WIO_SD_BUSY;
  }
  wio_sd_result_t result = sd_validate_io(lba, buffer, count);
  if (result == WIO_SD_OK && (!sd_info.writable || sd_info.write_protected)) {
    result = WIO_SD_WRITE_PROTECTED;
  }
  if (result != WIO_SD_OK) {
    sd_busy = false;
    return result;
  }
  const uint8_t *in = buffer;
  for (uint32_t i = 0U; i < count; ++i) {
    const uint32_t address = sd_info.block_addressed ? lba : lba * WIO_SD_SECTOR_SIZE;
    const uint8_t token = 0xFEU;
    memcpy(sd_sector, in, WIO_SD_SECTOR_SIZE);
    const uint16_t crc = sd_crc16(sd_sector, WIO_SD_SECTOR_SIZE);
    const uint8_t trailer[2] = {(uint8_t)(crc >> 8), (uint8_t)crc};
    result = sd_command_ready(SD_CMD24, address);
    if (result == WIO_SD_OK) {
      result = sd_send(&token, 1U);
    }
    if (result == WIO_SD_OK) {
      result = sd_send(sd_sector, WIO_SD_SECTOR_SIZE);
    }
    if (result == WIO_SD_OK) {
      result = sd_send(trailer, sizeof(trailer));
    }
    uint8_t response = 0xFFU;
    if (result == WIO_SD_OK) {
      /* A bounded response search also accepts cards with a short fill delay. */
      for (uint32_t poll = 0U; poll < 10U; ++poll) {
        result = sd_receive(&response, 1U);
        if ((result != WIO_SD_OK) || (response != 0xFFU)) {
          break;
        }
      }
    }
    if (result == WIO_SD_OK) {
      const uint8_t status = response & 0x1FU;
      result = (status == 0x05U) ? WIO_SD_OK :
               (status == 0x0BU) ? WIO_SD_CRC :
               (response == 0xFFU) ? WIO_SD_TIMEOUT : WIO_SD_IO;
    }
    if (result == WIO_SD_OK) {
      result = sd_wait_ready();
    }
    if (result == WIO_SD_OK) {
      result = sd_command_ready(SD_CMD13, 0U);
    }
    if (result == WIO_SD_OK) {
      result = sd_receive(&response, 1U); /* R2's second status byte. */
      if ((result == WIO_SD_OK) && (response != 0U)) {
        result = WIO_SD_IO;
      }
    }
    if (result == WIO_SD_OK) {
      result = sd_deselect();
    }
    if (result != WIO_SD_OK) {
      return sd_finish(result); /* Never retry an ambiguous write. */
    }
    in += WIO_SD_SECTOR_SIZE;
    ++lba;
  }
  return sd_finish(WIO_SD_OK);
}

wio_sd_result_t wio_sd_sync(void) {
  if (!sd_claim()) {
    return WIO_SD_BUSY;
  }
  if (!sd_ready) {
    sd_busy = false;
    return WIO_SD_NOT_READY;
  }
  wio_sd_result_t result = sd_select();
  if (result == WIO_SD_OK) {
    result = sd_deselect();
  }
  return sd_finish(result);
}
