/* SPDX-License-Identifier: Apache-2.0 */
/* Read-only, synchronous CMSIS Flash driver using GPIO single-lane SPI. */
#include "wio_flash.h"

#include <stddef.h>

#include "wio_sdk.h"
#include "wio_terminal.h"

/*
 * Winbond W25Q32JV Rev. F, May 11 2017 (documentation id user/0900766b81622f8f).
 * PDF pp.5, 36, 38: geometry and erased value; pp.10, 12: pins and mode 0;
 * pp.14, 17, 26: status; pp.22, 50: IDs; p.29: 03h + 24-bit address;
 * pp.64-65: timing. Only read/status opcodes are implemented below.
 */
#define FLASH_READ_DATA       0x03U
#define FLASH_READ_STATUS1    0x05U
#define FLASH_READ_STATUS2    0x35U
#define FLASH_READ_JEDEC_ID   0x9FU
#define FLASH_STATUS1_BUSY    0x01U
#define FLASH_STATUS2_SUS     0x80U
#define FLASH_SECTOR_BYTES    4096U
#define FLASH_PAGE_BYTES       256U

static volatile bool flash_initialized;
static volatile bool flash_powered;
static volatile bool flash_busy;
static volatile bool flash_error;
static volatile uint32_t flash_generation;

/* Programming fields describe the silicon only; this driver cannot write it. */
static ARM_FLASH_INFO flash_info = {
  .sector_info = NULL,
  .sector_count = WIO_FLASH_SIZE_BYTES / FLASH_SECTOR_BYTES,
  .sector_size = FLASH_SECTOR_BYTES,
  .page_size = FLASH_PAGE_BYTES,
  .program_unit = 1U,
  .erased_value = 0xFFU
};

static uint32_t flash_lock(void) {
  const uint32_t key = __get_PRIMASK();
  __disable_irq();
  __DMB();
  return key;
}

static void flash_unlock(uint32_t key) {
  __DMB();
  __set_PRIMASK(key);
}

/* Caller holds the short critical section; old cancelled operations stay inert. */
static bool flash_active(uint32_t generation) {
  return flash_initialized && flash_powered && flash_busy &&
         (flash_generation == generation);
}

static int32_t flash_begin(uint32_t *generation) {
  const uint32_t key = flash_lock();
  if (!flash_initialized || !flash_powered) {
    flash_unlock(key);
    return ARM_DRIVER_ERROR;
  }
  if (flash_busy) {
    flash_unlock(key);
    return ARM_DRIVER_ERROR_BUSY;
  }
  flash_busy = true;
  flash_error = false;
  *generation = ++flash_generation;
  flash_unlock(key);
  return ARM_DRIVER_OK;
}

static int32_t flash_finish(uint32_t generation, int32_t result) {
  const uint32_t key = flash_lock();
  if (!flash_active(generation)) {
    flash_unlock(key);
    return ARM_DRIVER_ERROR;
  }
  wio_gpio_write(WIO_PIN_QSPI_SCK, 0U);
  wio_gpio_write(WIO_PIN_QSPI_CS, 1U);
  wio_delay_us(1U); /* p.64: deselect time for reads >= 10 ns. */
  flash_error = result < 0;
  flash_busy = false;
  flash_unlock(key);
  return result;
}

static bool flash_select(uint32_t generation, bool selected) {
  const uint32_t key = flash_lock();
  if (!flash_active(generation)) {
    flash_unlock(key);
    return false;
  }
  wio_gpio_write(WIO_PIN_QSPI_SCK, 0U);
  wio_gpio_write(WIO_PIN_QSPI_CS, selected ? 0U : 1U);
  wio_delay_us(1U);
  flash_unlock(key);
  return true;
}

static bool flash_byte(uint32_t generation, uint8_t tx, uint8_t *rx) {
  uint8_t received = 0U;

  for (uint32_t bit = 0U; bit < 8U; ++bit) {
    const uint32_t key = flash_lock();
    if (!flash_active(generation)) {
      flash_unlock(key);
      return false;
    }

    /* Mode 0, MSB first. DWT delay does not depend on interrupt service. */
    wio_gpio_write(WIO_PIN_QSPI_IO0, (tx & 0x80U) != 0U);
    wio_delay_us(1U);
    wio_gpio_write(WIO_PIN_QSPI_SCK, 1U);
    wio_delay_us(1U);
    received = (uint8_t)((received << 1) | wio_gpio_read(WIO_PIN_QSPI_IO1));
    wio_gpio_write(WIO_PIN_QSPI_SCK, 0U);
    tx = (uint8_t)(tx << 1);

    /* A cancellation cannot write the caller's buffer after PowerControl(OFF). */
    if ((bit == 7U) && (rx != NULL)) {
      *rx = received;
    }
    flash_unlock(key);
  }
  return true;
}

static bool flash_read_register(uint32_t generation, uint8_t command,
                                uint8_t *value) {
  return flash_select(generation, true) &&
         flash_byte(generation, command, NULL) &&
         flash_byte(generation, 0xFFU, value) &&
         flash_select(generation, false);
}

static int32_t flash_check_idle(uint32_t generation) {
  uint8_t sr1;
  uint8_t sr2;

  if (!flash_read_register(generation, FLASH_READ_STATUS1, &sr1)) {
    return ARM_DRIVER_ERROR;
  }
  if ((sr1 & FLASH_STATUS1_BUSY) != 0U) {
    return ARM_DRIVER_ERROR_BUSY;
  }
  if (!flash_read_register(generation, FLASH_READ_STATUS2, &sr2)) {
    return ARM_DRIVER_ERROR;
  }
  /* Do not read an address that could belong to a suspended erase/program. */
  return (sr2 & FLASH_STATUS2_SUS) != 0U ? ARM_DRIVER_ERROR_BUSY : ARM_DRIVER_OK;
}

static bool flash_read_jedec(uint32_t generation, uint8_t id[3]) {
  return flash_select(generation, true) &&
         flash_byte(generation, FLASH_READ_JEDEC_ID, NULL) &&
         flash_byte(generation, 0xFFU, &id[0]) &&
         flash_byte(generation, 0xFFU, &id[1]) &&
         flash_byte(generation, 0xFFU, &id[2]) &&
         flash_select(generation, false);
}

static bool flash_known_id(const uint8_t id[3]) {
  return (id[0] == 0xEFU) && (id[2] == 0x16U) &&
         ((id[1] == 0x40U) || (id[1] == 0x70U));
}

/* Caller holds the lock. There are no native QSPI/DMA/interrupt resources. */
static void flash_power_off(void) {
  const bool was_powered = flash_powered;
  ++flash_generation;
  flash_powered = false;
  flash_busy = false;
  flash_error = false;

  if (was_powered) {
    wio_gpio_write(WIO_PIN_QSPI_CS, 1U);
    wio_gpio_write(WIO_PIN_QSPI_SCK, 0U);
    wio_gpio_input(WIO_PIN_QSPI_CS, 1);
    wio_gpio_input(WIO_PIN_QSPI_SCK, -1);
    wio_gpio_input(WIO_PIN_QSPI_IO0, 0);
    wio_gpio_input(WIO_PIN_QSPI_IO1, 0);
    wio_gpio_input(WIO_PIN_QSPI_IO2, 1);
    wio_gpio_input(WIO_PIN_QSPI_IO3, 1);
  }
}

static ARM_DRIVER_VERSION flash_version(void) {
  const ARM_DRIVER_VERSION version = {
    ARM_FLASH_API_VERSION, ARM_DRIVER_VERSION_MAJOR_MINOR(1, 0)
  };
  return version;
}

static ARM_FLASH_CAPABILITIES flash_capabilities(void) {
  ARM_FLASH_CAPABILITIES capabilities = {0};
  /* event_ready=0, data_width=0 (8-bit items), erase_chip=0. */
  return capabilities;
}

static int32_t flash_initialize(ARM_Flash_SignalEvent_t callback) {
  (void)callback; /* Synchronous implementation: no READY/ERROR events. */
  const uint32_t key = flash_lock();
  if (flash_busy) {
    flash_unlock(key);
    return ARM_DRIVER_ERROR_BUSY;
  }
  if (!flash_initialized) {
    flash_initialized = true;
    flash_error = false;
  }
  flash_unlock(key);
  return ARM_DRIVER_OK;
}

static int32_t flash_uninitialize(void) {
  const uint32_t key = flash_lock();
  flash_power_off();
  flash_initialized = false;
  flash_unlock(key);
  return ARM_DRIVER_OK;
}

static int32_t flash_power(ARM_POWER_STATE state) {
  if (state == ARM_POWER_LOW) {
    return ARM_DRIVER_ERROR_UNSUPPORTED;
  }
  if ((state != ARM_POWER_FULL) && (state != ARM_POWER_OFF)) {
    return ARM_DRIVER_ERROR_PARAMETER;
  }

  const uint32_t key = flash_lock();
  if (state == ARM_POWER_OFF) {
    flash_power_off();
    flash_unlock(key);
    return ARM_DRIVER_OK;
  }
  if (!flash_initialized) {
    flash_unlock(key);
    return ARM_DRIVER_ERROR;
  }
  if (!flash_powered) {
    /* Select high first. Do not assert /HOLD or /WP or change QE/protection. */
    wio_gpio_output(WIO_PIN_QSPI_CS, 1U);
    wio_gpio_output(WIO_PIN_QSPI_SCK, 0U);
    wio_gpio_output(WIO_PIN_QSPI_IO0, 1U);
    wio_gpio_input(WIO_PIN_QSPI_IO1, 0);
    /* Pull-ups avoid driving against an unexpected prior quad configuration. */
    wio_gpio_input(WIO_PIN_QSPI_IO2, 1);
    wio_gpio_input(WIO_PIN_QSPI_IO3, 1);
    wio_delay_us(1U);
    ++flash_generation;
    flash_error = false;
    flash_powered = true;
  }
  flash_unlock(key);
  return ARM_DRIVER_OK;
}

static int32_t flash_read(uint32_t address, void *data, uint32_t count) {
  uint8_t id[3];
  uint32_t generation;

  if ((data == NULL) || (count == 0U) || (address >= WIO_FLASH_SIZE_BYTES) ||
      (count > WIO_FLASH_SIZE_BYTES - address)) {
    return ARM_DRIVER_ERROR_PARAMETER;
  }
  int32_t result = flash_begin(&generation);
  if (result != ARM_DRIVER_OK) {
    return result;
  }
  result = flash_check_idle(generation);
  if (result != ARM_DRIVER_OK) {
    return flash_finish(generation, result);
  }
  if (!flash_read_jedec(generation, id)) {
    return flash_finish(generation, ARM_DRIVER_ERROR);
  }
  if (!flash_known_id(id)) {
    return flash_finish(generation, ARM_DRIVER_ERROR_UNSUPPORTED);
  }
  if (!flash_select(generation, true) ||
      !flash_byte(generation, FLASH_READ_DATA, NULL) ||
      !flash_byte(generation, (uint8_t)(address >> 16), NULL) ||
      !flash_byte(generation, (uint8_t)(address >> 8), NULL) ||
      !flash_byte(generation, (uint8_t)address, NULL)) {
    return flash_finish(generation, ARM_DRIVER_ERROR);
  }

  uint8_t *const bytes = (uint8_t *)data;
  for (uint32_t i = 0U; i < count; ++i) {
    if (!flash_byte(generation, 0xFFU, &bytes[i])) {
      return flash_finish(generation, ARM_DRIVER_ERROR);
    }
  }
  return flash_finish(generation, (int32_t)count);
}

static int32_t flash_program(uint32_t address, const void *data, uint32_t count) {
  (void)address;
  (void)data;
  (void)count;
  return ARM_DRIVER_ERROR_UNSUPPORTED;
}

static int32_t flash_erase_sector(uint32_t address) {
  (void)address;
  return ARM_DRIVER_ERROR_UNSUPPORTED;
}

static int32_t flash_erase_chip(void) {
  return ARM_DRIVER_ERROR_UNSUPPORTED;
}

static ARM_FLASH_STATUS flash_status(void) {
  ARM_FLASH_STATUS status = {0};
  const uint32_t key = flash_lock();
  status.busy = flash_busy;
  status.error = flash_error;
  flash_unlock(key);
  return status;
}

static ARM_FLASH_INFO *flash_get_info(void) {
  return &flash_info;
}

bool wio_flash_read_id(uint8_t id[3]) {
  uint8_t sampled[3];
  uint32_t generation;

  if ((id == NULL) || (flash_begin(&generation) != ARM_DRIVER_OK)) {
    return false;
  }
  int32_t result = flash_check_idle(generation);
  if (result != ARM_DRIVER_OK) {
    (void)flash_finish(generation, result);
    return false;
  }
  if (!flash_read_jedec(generation, sampled)) {
    /* A cancelled generation cannot deselect or unlock a newer operation. */
    (void)flash_finish(generation, ARM_DRIVER_ERROR);
    return false;
  }

  /* Publish all three bytes before OFF/Uninitialize can reuse the output. */
  const uint32_t key = flash_lock();
  result = flash_finish(generation, ARM_DRIVER_OK); /* Keeps outer IRQ mask. */
  if (result == ARM_DRIVER_OK) {
    id[0] = sampled[0];
    id[1] = sampled[1];
    id[2] = sampled[2];
  }
  flash_unlock(key);
  return result == ARM_DRIVER_OK;
}

ARM_DRIVER_FLASH Driver_Flash0 = {
  .GetVersion = flash_version,
  .GetCapabilities = flash_capabilities,
  .Initialize = flash_initialize,
  .Uninitialize = flash_uninitialize,
  .PowerControl = flash_power,
  .ReadData = flash_read,
  .ProgramData = flash_program,
  .EraseSector = flash_erase_sector,
  .EraseChip = flash_erase_chip,
  .GetStatus = flash_status,
  .GetInfo = flash_get_info
};
