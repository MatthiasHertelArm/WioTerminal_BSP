/* SPDX-License-Identifier: Apache-2.0 */
/* Real SDK SD code, genuine CMSIS SPI ABI, deterministic asynchronous card model. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "Driver_SPI.h"
#include "wio_sd.h"
#include "wio_terminal.h"

#define CHECK(condition) do { if (!(condition)) { \
  fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

uint32_t storage_test_ipsr;
uint32_t storage_test_primask;
uint32_t storage_test_basepri;
uint32_t storage_test_faultmask;

static struct {
  bool version2, block_addressed, protected_card, idle, crc_enabled;
  bool bad_r7, bad_ocr, bad_csd_crc, bad_data_crc, no_data, reject_crc_mode;
  bool acmd_stuck, spi_hang, wrong_count, data_lost, frozen_time;
  bool card_busy, write_busy_stuck, write_completed, stored_valid;
  bool selected, powered;
  uint8_t csd_structure, read_length, write_response, second_status;
  uint32_t csd_size, csd_mult, command_count[64], last_address;
  uint32_t write_lba, stored_lba, first_command_clocks, high_clocks;
  uint32_t now, status_polls, aborts, bus_hz, init_hz, default_tx;
  uint8_t response[600], stored[512], pending_write[512];
  size_t response_pos, response_end;
  uint32_t write_stage;
  const uint8_t *pending_tx;
  uint8_t *pending_rx;
  uint32_t pending_num, completed_num, pending_polls;
} card;

uint32_t wio_millis(void) { return card.frozen_time ? card.now : card.now++; }
void wio_delay_ms(uint32_t ms) { if (!card.frozen_time) card.now += ms; }
void wio_delay_us(uint32_t us) { if (!card.frozen_time) card.now += us / 1000U; }

void wio_gpio_write(uint8_t pin, uint32_t level) {
  CHECK(pin == WIO_PIN_SD_CS);
  card.selected = level == 0U;
  if (!card.selected) {
    card.response_pos = card.response_end = 0U;
    card.write_stage = 0U;
  }
}
void wio_gpio_output(uint8_t pin, uint32_t level) { wio_gpio_write(pin, level); }

/* Polynomial long division, independent from the SDK's shift-register CRC7. */
static uint8_t command_crc(const uint8_t frame[5]) {
  uint64_t remainder = 0U;
  for (unsigned i = 0U; i < 5U; ++i) remainder = (remainder << 8) | frame[i];
  remainder <<= 7;
  for (int bit = 46; bit >= 7; --bit) {
    if ((remainder & (UINT64_C(1) << bit)) != 0U) remainder ^= UINT64_C(0x89) << (bit - 7);
  }
  return (uint8_t)((remainder << 1) | 1U);
}

/* Nibble-fold CRC-CCITT; cross-checked against the standard 123456789 vector. */
static uint16_t data_crc(const uint8_t *p, size_t count) {
  uint16_t crc = 0U;
  while (count-- != 0U) {
    crc = (uint16_t)((crc >> 8) | (crc << 8));
    crc ^= *p++;
    crc ^= (uint16_t)((crc & 255U) >> 4);
    crc ^= (uint16_t)(crc << 12);
    crc ^= (uint16_t)((crc & 255U) << 5);
  }
  return crc;
}

static void enqueue(uint8_t byte) {
  CHECK(card.response_end < sizeof(card.response));
  card.response[card.response_end++] = byte;
}
static void enqueue_packet(const uint8_t *data, size_t count, bool bad_crc) {
  enqueue(0xFEU);
  for (size_t i = 0U; i < count; ++i) enqueue(data[i]);
  const uint16_t crc = (uint16_t)(data_crc(data, count) ^ (bad_crc ? 1U : 0U));
  enqueue((uint8_t)(crc >> 8)); enqueue((uint8_t)crc);
}
static void command(const uint8_t *frame, uint32_t count) {
  CHECK(count == 6U && (frame[0] & 0xC0U) == 0x40U);
  CHECK(frame[5] == command_crc(frame));
  const uint8_t cmd = frame[0] & 0x3FU;
  const uint32_t arg = ((uint32_t)frame[1] << 24) | ((uint32_t)frame[2] << 16) |
                        ((uint32_t)frame[3] << 8) | frame[4];
  ++card.command_count[cmd];
  if (cmd == 0U) {
    CHECK(frame[5] == 0x95U && arg == 0U);
    CHECK(card.bus_hz <= 400000U);
    if (card.command_count[cmd] == 1U) card.first_command_clocks = card.high_clocks;
    card.idle = true; card.crc_enabled = false; enqueue(1U);
  } else if (cmd == 59U) {
    CHECK(arg == 1U); card.crc_enabled = !card.reject_crc_mode;
    enqueue(card.reject_crc_mode ? 5U : 1U);
  } else if (cmd == 8U) {
    CHECK(frame[5] == 0x87U && arg == 0x1AAU && card.crc_enabled);
    enqueue(card.version2 ? 1U : 5U);
    if (card.version2) {
      enqueue(0U); enqueue(0U); enqueue(1U); enqueue(card.bad_r7 ? 0xABU : 0xAAU);
    }
  } else if (cmd == 55U) {
    enqueue(card.idle ? 1U : 0U);
  } else if (cmd == 41U) {
    CHECK(arg == (card.version2 ? 0x40000000U : 0U));
    if (!card.acmd_stuck) card.idle = false;
    enqueue(card.idle ? 1U : 0U);
  } else if (cmd == 58U) {
    enqueue(0U);
    enqueue(card.bad_ocr ? 0U : (card.block_addressed ? 0xC0U : 0x80U));
    enqueue(0xFFU); enqueue(0x80U); enqueue(0U);
  } else if (cmd == 16U) {
    CHECK(!card.block_addressed && arg == 512U); enqueue(0U);
  } else if (cmd == 9U) {
    uint8_t csd[16] = {0};
    csd[0] = (uint8_t)(card.csd_structure << 6);
    csd[5] = card.read_length;
    if (card.csd_structure == 0U) {
      csd[6] = (uint8_t)((card.csd_size >> 10) & 3U);
      csd[7] = (uint8_t)(card.csd_size >> 2);
      csd[8] = (uint8_t)(card.csd_size << 6);
      csd[9] = (uint8_t)(card.csd_mult >> 1);
      csd[10] = (uint8_t)(card.csd_mult << 7);
    } else {
      csd[7] = (uint8_t)((card.csd_size >> 16) & 63U);
      csd[8] = (uint8_t)(card.csd_size >> 8); csd[9] = (uint8_t)card.csd_size;
    }
    csd[14] = card.protected_card ? 0x10U : 0U;
    csd[15] = 1U;
    enqueue(0U); enqueue_packet(csd, sizeof(csd), card.bad_csd_crc);
  } else if (cmd == 17U) {
    card.last_address = arg;
    const uint32_t lba = card.block_addressed ? arg : arg / 512U;
    if (!card.block_addressed) CHECK((arg % 512U) == 0U);
    uint8_t data[512];
    for (uint32_t i = 0U; i < sizeof(data); ++i) data[i] = (uint8_t)(lba + i);
    enqueue(0U);
    if (!card.no_data) enqueue_packet(card.stored_valid && card.stored_lba == lba ?
                                      card.stored : data, sizeof(data), card.bad_data_crc);
  } else if (cmd == 24U) {
    card.last_address = arg;
    card.write_lba = card.block_addressed ? arg : arg / 512U;
    if (!card.block_addressed) CHECK((arg % 512U) == 0U);
    card.write_stage = 1U; card.write_completed = false; enqueue(0U);
  } else if (cmd == 13U) {
    enqueue(0U); enqueue(card.second_status);
  } else {
    CHECK(!"Unexpected command: erase/trim/multiblock commands are forbidden");
  }
}

static void send_bytes(const uint8_t *p, uint32_t count) {
  CHECK(card.selected);
  if (card.write_stage == 1U) {
    CHECK(count == 1U && p[0] == 0xFEU); card.write_stage = 2U;
  } else if (card.write_stage == 2U) {
    CHECK(count == 512U); memcpy(card.pending_write, p, count); card.write_stage = 3U;
  } else if (card.write_stage == 3U) {
    CHECK(count == 2U);
    CHECK(((uint16_t)p[0] << 8 | p[1]) == data_crc(card.pending_write, 512U));
    if (card.write_response == 0x05U) {
      memcpy(card.stored, card.pending_write, 512U);
      card.stored_lba = card.write_lba; card.stored_valid = true;
    }
    enqueue(card.write_response);
    if (!card.write_busy_stuck) { enqueue(0U); enqueue(0U); enqueue(0xFFU); }
    card.write_stage = 0U; card.write_completed = true;
  } else {
    command(p, count);
  }
}

static uint8_t receive_byte(void) {
  if (!card.selected) { card.high_clocks += 8U; return 0xFFU; }
  if (card.response_pos != card.response_end) return card.response[card.response_pos++];
  if (card.card_busy || (card.write_completed && card.write_busy_stuck)) return 0U;
  return 0xFFU;
}

static ARM_DRIVER_VERSION mock_version(void) { ARM_DRIVER_VERSION v = {0}; return v; }
static ARM_SPI_CAPABILITIES mock_capabilities(void) { ARM_SPI_CAPABILITIES c = {0}; return c; }
static int32_t mock_initialize(ARM_SPI_SignalEvent_t callback) {
  CHECK(callback == NULL); return ARM_DRIVER_OK;
}
static int32_t mock_uninitialize(void) { card.powered = false; return ARM_DRIVER_OK; }
static int32_t mock_power(ARM_POWER_STATE power) {
  card.powered = power == ARM_POWER_FULL;
  if (!card.powered) {
    card.pending_num = 0U; card.pending_tx = NULL; card.pending_rx = NULL;
  }
  return ARM_DRIVER_OK;
}
static int32_t mock_send(const void *p, uint32_t count) {
  CHECK(card.powered && p != NULL && count > 0U && card.pending_num == 0U);
  card.pending_tx = p; card.pending_rx = NULL;
  card.pending_num = count; card.pending_polls = 0U;
  return ARM_DRIVER_OK;
}
static int32_t mock_receive(void *p, uint32_t count) {
  CHECK(card.powered && card.default_tx == 0xFFU && p != NULL && count > 0U && card.pending_num == 0U);
  card.pending_rx = p; card.pending_tx = NULL;
  card.pending_num = count; card.pending_polls = 0U;
  return ARM_DRIVER_OK;
}
static int32_t mock_transfer(const void *out, void *in, uint32_t count) {
  (void)out; (void)in; (void)count; CHECK(!"Unexpected Transfer"); return ARM_DRIVER_ERROR;
}
static uint32_t mock_count(void) { return card.wrong_count ? 0U : card.completed_num; }
static int32_t mock_control(uint32_t control, uint32_t arg) {
  switch (control & ARM_SPI_CONTROL_Msk) {
    case ARM_SPI_MODE_MASTER:
      CHECK(control == (ARM_SPI_MODE_MASTER | ARM_SPI_CPOL0_CPHA0 | ARM_SPI_DATA_BITS(8U) |
                        ARM_SPI_MSB_LSB | ARM_SPI_SS_MASTER_UNUSED));
      card.bus_hz = card.init_hz = arg; return ARM_DRIVER_OK;
    case ARM_SPI_SET_BUS_SPEED: card.bus_hz = arg; return ARM_DRIVER_OK;
    case ARM_SPI_SET_DEFAULT_TX_VALUE: card.default_tx = arg; return ARM_DRIVER_OK;
    case ARM_SPI_ABORT_TRANSFER:
      ++card.aborts; card.pending_num = 0U;
      card.pending_tx = NULL; card.pending_rx = NULL; return ARM_DRIVER_OK;
    default: CHECK(!"Unexpected SPI control"); return ARM_DRIVER_ERROR;
  }
}
static ARM_SPI_STATUS mock_status(void) {
  ARM_SPI_STATUS status = {0};
  ++card.status_polls;
  if (card.pending_num != 0U) {
    if (card.spi_hang || card.data_lost || card.pending_polls++ == 0U) {
      status.busy = 1U; status.data_lost = card.data_lost; return status;
    }
    if (card.pending_tx != NULL) send_bytes(card.pending_tx, card.pending_num);
    else for (uint32_t i = 0U; i < card.pending_num; ++i) card.pending_rx[i] = receive_byte();
    card.completed_num = card.pending_num; card.pending_num = 0U;
    card.pending_tx = NULL; card.pending_rx = NULL;
  }
  return status;
}

ARM_DRIVER_SPI Driver_SPI6 = {
  mock_version, mock_capabilities, mock_initialize, mock_uninitialize,
  mock_power, mock_send, mock_receive, mock_transfer, mock_count, mock_control, mock_status
};

static void reset_card(bool version2, bool block_addressed) {
  storage_test_ipsr = storage_test_primask = storage_test_basepri = storage_test_faultmask = 0U;
  CHECK(wio_sd_deinit() == WIO_SD_OK);
  memset(&card, 0, sizeof(card));
  card.version2 = version2; card.block_addressed = block_addressed;
  card.csd_structure = block_addressed ? 1U : 0U;
  card.csd_size = block_addressed ? 0U : 255U;
  card.read_length = 9U;
  card.write_response = 0x05U;
}

static void test_read_and_write(void) {
  reset_card(true, true);
  CHECK(wio_sd_init() == WIO_SD_OK);
  wio_sd_info_t info;
  CHECK(wio_sd_get_info(&info) == WIO_SD_OK);
  CHECK(info.sector_count == 1024U && info.sector_size == 512U && info.block_addressed);
  CHECK(!info.writable && !info.write_protected && card.crc_enabled);
  CHECK(card.init_hz <= 400000U && card.bus_hz <= 4000000U && card.first_command_clocks >= 80U);
  uint8_t data[1024], copy[512];
  CHECK(wio_sd_read_blocks(7U, data, 2U) == WIO_SD_OK && card.last_address == 8U);
  for (unsigned i = 0U; i < 512U; ++i) {
    CHECK(data[i] == (uint8_t)(7U + i));
    CHECK(data[512U + i] == (uint8_t)(8U + i));
  }
  const uint32_t reads = card.command_count[17];
  CHECK(wio_sd_read_blocks(1023U, data, 2U) == WIO_SD_PARAMETER);
  CHECK(wio_sd_read_blocks(0U, NULL, 1U) == WIO_SD_PARAMETER);
  CHECK(wio_sd_read_blocks(0U, data, 0U) == WIO_SD_PARAMETER && card.command_count[17] == reads);
  CHECK(wio_sd_get_info(NULL) == WIO_SD_PARAMETER);
  CHECK(wio_sd_write_blocks(9U, data, 1U) == WIO_SD_WRITE_PROTECTED && card.command_count[24] == 0U);
  CHECK(wio_sd_set_writable(true) == WIO_SD_OK);
  memset(data, 0xA6, 512U);
  CHECK(wio_sd_write_blocks(9U, data, 1U) == WIO_SD_OK && card.command_count[13] == 1U);
  CHECK(wio_sd_read_blocks(9U, copy, 1U) == WIO_SD_OK && memcmp(data, copy, sizeof(copy)) == 0);
  CHECK(wio_sd_sync() == WIO_SD_OK);
  CHECK(wio_sd_set_writable(false) == WIO_SD_OK);
  CHECK(wio_sd_write_blocks(10U, data, 1U) == WIO_SD_WRITE_PROTECTED && card.command_count[24] == 1U);
  CHECK(wio_sd_set_writable(true) == WIO_SD_OK);
  CHECK(wio_sd_init() == WIO_SD_OK);
  CHECK(wio_sd_get_info(&info) == WIO_SD_OK && !info.writable);
}

static void test_addressing_and_capacity(void) {
  uint8_t data[512];
  wio_sd_info_t info;
  for (unsigned version2 = 0U; version2 < 2U; ++version2) {
    reset_card(version2 != 0U, false);
    CHECK(wio_sd_init() == WIO_SD_OK && card.command_count[16] == 1U);
    CHECK(wio_sd_get_info(&info) == WIO_SD_OK && !info.block_addressed && info.sector_count == 1024U);
    CHECK(wio_sd_read_blocks(7U, data, 1U) == WIO_SD_OK && card.last_address == 7U * 512U);
    CHECK(wio_sd_set_writable(true) == WIO_SD_OK);
    CHECK(wio_sd_write_blocks(8U, data, 1U) == WIO_SD_OK && card.last_address == 8U * 512U);
  }
  reset_card(true, true); card.csd_size = 0x3FFFFFU;
  CHECK(wio_sd_init() == WIO_SD_OK);
  CHECK(wio_sd_get_info(&info) == WIO_SD_OK && info.sector_count == (UINT64_C(1) << 32));
  CHECK(wio_sd_read_blocks(UINT32_MAX, data, 1U) == WIO_SD_OK && card.last_address == UINT32_MAX);
  CHECK(wio_sd_read_blocks(UINT32_MAX, data, 2U) == WIO_SD_PARAMETER);
  reset_card(false, false); card.csd_size = 4095U; card.csd_mult = 7U; card.read_length = 11U;
  CHECK(wio_sd_init() == WIO_SD_OK);
  CHECK(wio_sd_get_info(&info) == WIO_SD_OK && info.sector_count == 8388608U);
  CHECK(wio_sd_read_blocks(8388607U, data, 1U) == WIO_SD_OK && card.last_address == 0xFFFFFE00U);
  CHECK(wio_sd_read_blocks(8388608U, data, 1U) == WIO_SD_PARAMETER);
}

static void test_initialization_failures(void) {
  reset_card(true, true); card.bad_r7 = true;
  CHECK(wio_sd_init() == WIO_SD_UNSUPPORTED);
  reset_card(true, true); card.bad_ocr = true;
  CHECK(wio_sd_init() == WIO_SD_UNSUPPORTED);
  reset_card(true, true); card.csd_structure = 2U;
  CHECK(wio_sd_init() == WIO_SD_UNSUPPORTED);
  reset_card(false, false); card.read_length = 15U;
  CHECK(wio_sd_init() == WIO_SD_UNSUPPORTED);
  reset_card(true, true); card.reject_crc_mode = true;
  CHECK(wio_sd_init() == WIO_SD_UNSUPPORTED);
  reset_card(true, true); card.bad_csd_crc = true;
  CHECK(wio_sd_init() == WIO_SD_CRC);
  reset_card(true, true); card.acmd_stuck = true; card.frozen_time = true;
  CHECK(wio_sd_init() == WIO_SD_TIMEOUT && card.command_count[41] <= 2000U);
  reset_card(true, true); card.card_busy = true; card.frozen_time = true;
  CHECK(wio_sd_init() == WIO_SD_TIMEOUT && card.command_count[0] == 0U);
  CHECK(card.status_polls <= 40020U); /* One bounded ready wait, not 2000 nested retries. */
}

static void test_write_protection_and_errors(void) {
  uint8_t data[512] = {0};
  wio_sd_info_t info;
  reset_card(true, true); card.protected_card = true;
  CHECK(wio_sd_init() == WIO_SD_OK && wio_sd_get_info(&info) == WIO_SD_OK && info.write_protected);
  CHECK(wio_sd_set_writable(true) == WIO_SD_WRITE_PROTECTED);
  CHECK(wio_sd_write_blocks(0U, data, 1U) == WIO_SD_WRITE_PROTECTED && card.command_count[24] == 0U);
  CHECK(wio_sd_read_blocks(0U, data, 1U) == WIO_SD_OK);

  reset_card(true, true); CHECK(wio_sd_init() == WIO_SD_OK);
  CHECK(wio_sd_set_writable(true) == WIO_SD_OK); card.write_response = 0x0BU;
  CHECK(wio_sd_write_blocks(0U, data, 1U) == WIO_SD_CRC && card.command_count[24] == 1U);
  CHECK(wio_sd_get_info(&info) == WIO_SD_NOT_READY);
  CHECK(wio_sd_set_writable(true) == WIO_SD_NOT_READY);

  reset_card(true, true); CHECK(wio_sd_init() == WIO_SD_OK);
  CHECK(wio_sd_set_writable(true) == WIO_SD_OK); card.write_busy_stuck = true;
  CHECK(wio_sd_write_blocks(0U, data, 1U) == WIO_SD_TIMEOUT && card.command_count[24] == 1U);
  CHECK(card.command_count[13] == 0U); /* No retry or status command while card remains busy. */

  reset_card(true, true); CHECK(wio_sd_init() == WIO_SD_OK);
  CHECK(wio_sd_set_writable(true) == WIO_SD_OK); card.second_status = 0x20U;
  CHECK(wio_sd_write_blocks(0U, data, 1U) == WIO_SD_IO && card.command_count[24] == 1U);
}

static void test_transport_failures_and_context(void) {
  uint8_t data[512];
  wio_sd_info_t info;
  reset_card(true, true); CHECK(wio_sd_init() == WIO_SD_OK);
  memset(data, 0xA5, sizeof(data)); card.bad_data_crc = true;
  CHECK(wio_sd_read_blocks(0U, data, 1U) == WIO_SD_CRC);
  for (unsigned i = 0U; i < sizeof(data); ++i) CHECK(data[i] == 0xA5U);
  CHECK(wio_sd_get_info(&info) == WIO_SD_NOT_READY);

  reset_card(true, true); CHECK(wio_sd_init() == WIO_SD_OK); card.no_data = true;
  CHECK(wio_sd_read_blocks(0U, data, 1U) == WIO_SD_TIMEOUT);
  reset_card(true, true); CHECK(wio_sd_init() == WIO_SD_OK); card.wrong_count = true;
  CHECK(wio_sd_read_blocks(0U, data, 1U) == WIO_SD_IO);
  reset_card(true, true); CHECK(wio_sd_init() == WIO_SD_OK); card.data_lost = true;
  CHECK(wio_sd_read_blocks(0U, data, 1U) == WIO_SD_IO);
  reset_card(true, true); CHECK(wio_sd_init() == WIO_SD_OK);
  card.spi_hang = true; card.frozen_time = true;
  const uint32_t polls = card.status_polls;
  CHECK(wio_sd_read_blocks(0U, data, 1U) == WIO_SD_TIMEOUT);
  CHECK(card.status_polls - polls <= 2000010U && card.aborts != 0U && !card.powered);

  reset_card(true, true); CHECK(wio_sd_init() == WIO_SD_OK);
  CHECK(wio_sd_set_writable(true) == WIO_SD_OK);
  const uint32_t commands = card.command_count[17];
  storage_test_ipsr = 1U;
  CHECK(wio_sd_read_blocks(0U, data, 1U) == WIO_SD_BUSY);
  CHECK(wio_sd_init() == WIO_SD_BUSY && wio_sd_deinit() == WIO_SD_BUSY);
  storage_test_ipsr = 0U; storage_test_basepri = 1U;
  CHECK(wio_sd_read_blocks(0U, data, 1U) == WIO_SD_BUSY);
  storage_test_basepri = 0U; storage_test_faultmask = 1U;
  CHECK(wio_sd_sync() == WIO_SD_BUSY);
  storage_test_faultmask = 0U; storage_test_primask = 1U;
  CHECK(wio_sd_set_writable(false) == WIO_SD_BUSY);
  storage_test_primask = 0U;
  CHECK(wio_sd_get_info(&info) == WIO_SD_OK && info.writable && card.command_count[17] == commands);
  card.card_busy = true; card.frozen_time = true;
  CHECK(wio_sd_sync() == WIO_SD_TIMEOUT);
}

int main(void) {
  const uint8_t cmd0[5] = {0x40U, 0U, 0U, 0U, 0U};
  const uint8_t cmd8[5] = {0x48U, 0U, 0U, 1U, 0xAAU};
  CHECK(command_crc(cmd0) == 0x95U && command_crc(cmd8) == 0x87U);
  CHECK(data_crc((const uint8_t *)"123456789", 9U) == 0x31C3U);
  test_read_and_write();
  test_addressing_and_capacity();
  test_initialization_failures();
  test_write_protection_and_errors();
  test_transport_failures_and_context();
  CHECK(wio_sd_deinit() == WIO_SD_OK);
  puts("storage_sd: CRC/addressing/protection/async-timeout tests passed");
  return 0;
}
