/* SPDX-License-Identifier: Apache-2.0 */
/* Independently implemented against the CMSIS-Driver I2C API. */
#include <stddef.h>
#include "Driver_I2C.h"
#include "wio_i2c.h"
#include "wio_sdk.h"
#include "wio_terminal.h"

#ifndef WIO_I2C_TIMEOUT_MS
#define WIO_I2C_TIMEOUT_MS 100U
#endif
#ifndef WIO_I2C_POLL_BUDGET
#define WIO_I2C_POLL_BUDGET 1000000U
#endif

extern ARM_DRIVER_I2C Driver_I2C3;
extern ARM_DRIVER_I2C Driver_I2C4;

static bool bus_ready[2];

static ARM_DRIVER_I2C *driver_for(wio_i2c_bus_t bus) {
  if (bus == WIO_I2C_GROVE) {
    return &Driver_I2C3;
  }
  if (bus == WIO_I2C_INTERNAL) {
    return &Driver_I2C4;
  }
  return NULL;
}

static bool thread_context(void) {
  return (__get_IPSR() == 0U) && (__get_PRIMASK() == 0U) &&
         (__get_BASEPRI() == 0U) && (__get_FAULTMASK() == 0U);
}

static ARM_DRIVER_I2C *ready_driver(wio_i2c_bus_t bus) {
  ARM_DRIVER_I2C *driver = driver_for(bus);

  if ((driver == NULL) || !thread_context()) {
    return NULL;
  }
  if (!bus_ready[(uint32_t)bus - (uint32_t)WIO_I2C_GROVE]) {
    return NULL;
  }
  return driver;
}

static bool transfer_complete(ARM_DRIVER_I2C *driver, uint32_t length) {
  const uint32_t start = wio_millis();
  uint32_t budget = WIO_I2C_POLL_BUDGET;
  ARM_I2C_STATUS status;

  do {
    status = driver->GetStatus();
    if (status.busy == 0U) {
      /* CMSIS specifies that transmit data count includes only ACKed bytes.
       * Thus a NACK, including address NACK, cannot pass this length check.
       */
      if ((status.bus_error == 0U) && (status.arbitration_lost == 0U) &&
          (driver->GetDataCount() == (int32_t)length)) {
        return true;
      }
      break;
    }
  } while ((--budget != 0U) &&
           ((uint32_t)(wio_millis() - start) < WIO_I2C_TIMEOUT_MS));

  /* Also release a repeated-START transaction whose first phase failed. */
  (void)driver->Control(ARM_I2C_ABORT_TRANSFER, 0U);
  return false;
}

bool wio_i2c_init(wio_i2c_bus_t bus) {
  ARM_DRIVER_I2C *driver = driver_for(bus);
  uint32_t index;

  if ((driver == NULL) || !thread_context()) {
    return false;
  }
  index = (uint32_t)bus - (uint32_t)WIO_I2C_GROVE;
  if (bus_ready[index]) {
    return true;
  }
  if (driver->Initialize(NULL) != ARM_DRIVER_OK) {
    return false;
  }
  if ((driver->PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK) ||
      (driver->Control(ARM_I2C_BUS_SPEED, ARM_I2C_BUS_SPEED_STANDARD) != ARM_DRIVER_OK)) {
    (void)driver->PowerControl(ARM_POWER_OFF);
    (void)driver->Uninitialize();
    return false;
  }
  bus_ready[index] = true;
  return true;
}

bool wio_i2c_uninit(wio_i2c_bus_t bus) {
  ARM_DRIVER_I2C *driver = ready_driver(bus);
  int32_t abort_result;
  int32_t power_result;
  int32_t uninit_result;

  if (driver == NULL) {
    return false;
  }
  abort_result = driver->Control(ARM_I2C_ABORT_TRANSFER, 0U);
  power_result = driver->PowerControl(ARM_POWER_OFF);
  uninit_result = driver->Uninitialize();
  bus_ready[(uint32_t)bus - (uint32_t)WIO_I2C_GROVE] = false;
  return (abort_result == ARM_DRIVER_OK) && (power_result == ARM_DRIVER_OK) &&
         (uninit_result == ARM_DRIVER_OK);
}

static bool valid_buffer(uint8_t address, const void *data, uint32_t length) {
  return (address <= 0x7FU) && (data != NULL) &&
         (length != 0U) && (length <= 0x7FFFFFFFU);
}

bool wio_i2c_write(wio_i2c_bus_t bus, uint8_t address,
                   const uint8_t *data, uint32_t length) {
  ARM_DRIVER_I2C *driver = ready_driver(bus);

  if ((driver == NULL) || !valid_buffer(address, data, length)) {
    return false;
  }
  if (driver->MasterTransmit(address, data, length, false) != ARM_DRIVER_OK) {
    return false;
  }
  return transfer_complete(driver, length);
}

bool wio_i2c_read(wio_i2c_bus_t bus, uint8_t address,
                  uint8_t *data, uint32_t length) {
  ARM_DRIVER_I2C *driver = ready_driver(bus);

  if ((driver == NULL) || !valid_buffer(address, data, length)) {
    return false;
  }
  if (driver->MasterReceive(address, data, length, false) != ARM_DRIVER_OK) {
    return false;
  }
  return transfer_complete(driver, length);
}

bool wio_i2c_write_read(wio_i2c_bus_t bus, uint8_t address,
                        const uint8_t *tx_data, uint32_t tx_length,
                        uint8_t *rx_data, uint32_t rx_length) {
  ARM_DRIVER_I2C *driver = ready_driver(bus);

  if ((driver == NULL) || !valid_buffer(address, tx_data, tx_length) ||
      !valid_buffer(address, rx_data, rx_length)) {
    return false;
  }
  if (driver->MasterTransmit(address, tx_data, tx_length, true) != ARM_DRIVER_OK) {
    return false;
  }
  if (!transfer_complete(driver, tx_length)) {
    return false;
  }
  if (driver->MasterReceive(address, rx_data, rx_length, false) != ARM_DRIVER_OK) {
    (void)driver->Control(ARM_I2C_ABORT_TRANSFER, 0U);
    return false;
  }
  return transfer_complete(driver, rx_length);
}
