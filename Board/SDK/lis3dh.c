/* SPDX-License-Identifier: Apache-2.0 */
/* Independent LIS3DH implementation. Register and I2C protocol reference:
 * ST Doc ID 17530 Rev 1, pp.21-22 and 29-34 (LIS3DH datasheet).
 */
#include <stddef.h>
#include "lis3dh.h"
#include "wio_i2c.h"
#include "wio_sdk.h"

#define LIS3DH_REG_WHO_AM_I     0x0FU
#define LIS3DH_REG_CTRL1        0x20U
#define LIS3DH_REG_CTRL2        0x21U
#define LIS3DH_REG_CTRL3        0x22U
#define LIS3DH_REG_CTRL4        0x23U
#define LIS3DH_REG_CTRL5        0x24U
#define LIS3DH_REG_CTRL6        0x25U
#define LIS3DH_REG_STATUS       0x27U
#define LIS3DH_REG_OUT_X_L      0x28U
#define LIS3DH_REG_FIFO_CTRL    0x2EU
#define LIS3DH_AUTO_INCREMENT   0x80U
#define LIS3DH_STATUS_XYZ_READY 0x08U
#define LIS3DH_CTRL1_100HZ_XYZ  0x57U
#define LIS3DH_CTRL4_BDU_HR_2G  0x88U
#define LIS3DH_DATA_TIMEOUT_MS  100U

static bool read_registers(uint8_t address, uint8_t reg,
                           uint8_t *data, uint32_t length) {
  const uint8_t command = (uint8_t)(reg | ((length > 1U) ? LIS3DH_AUTO_INCREMENT : 0U));
  return wio_i2c_write_read(WIO_I2C_INTERNAL, address, &command, 1U, data, length);
}

static bool write_register(uint8_t address, uint8_t reg, uint8_t value) {
  const uint8_t command[2] = { reg, value };
  return wio_i2c_write(WIO_I2C_INTERNAL, address, command, sizeof(command));
}

bool lis3dh_probe(lis3dh_t *device) {
  static const uint8_t addresses[2] = { 0x18U, 0x19U };

  if (device == NULL) {
    return false;
  }
  device->address = 0U;
  device->who_am_i = 0U;
  device->initialized = false;
  if (!wio_i2c_init(WIO_I2C_INTERNAL)) {
    return false;
  }
  for (uint32_t i = 0U; i < sizeof(addresses); ++i) {
    uint8_t identity = 0U;
    if (read_registers(addresses[i], LIS3DH_REG_WHO_AM_I, &identity, 1U) &&
        (identity == LIS3DH_WHO_AM_I_VALUE)) {
      device->address = addresses[i];
      device->who_am_i = identity;
      return true;
    }
  }
  return false;
}

bool lis3dh_init(lis3dh_t *device) {
  static const uint8_t setup[][2] = {
    { LIS3DH_REG_CTRL1,     0U },                  /* Power down before configuration. */
    { LIS3DH_REG_CTRL2,     0U },                  /* Keep gravity; no high-pass filter. */
    { LIS3DH_REG_CTRL3,     0U },                  /* INT1 disabled. */
    { LIS3DH_REG_CTRL4,     LIS3DH_CTRL4_BDU_HR_2G },
    { LIS3DH_REG_CTRL5,     0U },                  /* FIFO disabled; no reboot. */
    { LIS3DH_REG_CTRL6,     0U },                  /* INT2 disabled. */
    { LIS3DH_REG_FIFO_CTRL, 0U },                  /* FIFO bypass. */
    { LIS3DH_REG_CTRL1,     LIS3DH_CTRL1_100HZ_XYZ }
  };
  static const uint8_t expected[6] = {
    LIS3DH_CTRL1_100HZ_XYZ, 0U, 0U, LIS3DH_CTRL4_BDU_HR_2G, 0U, 0U
  };
  uint8_t actual[6];
  uint8_t discarded[6];

  if (!lis3dh_probe(device)) {
    return false;
  }
  for (uint32_t i = 0U; i < (sizeof(setup) / sizeof(setup[0])); ++i) {
    if (!write_register(device->address, setup[i][0], setup[i][1])) {
      return false;
    }
  }
  if (!read_registers(device->address, LIS3DH_REG_CTRL1, actual, sizeof(actual))) {
    return false;
  }
  for (uint32_t i = 0U; i < sizeof(expected); ++i) {
    if (actual[i] != expected[i]) {
      return false;
    }
  }
  /* Allow eight output periods for settling, then discard any old latched data
   * so that lis3dh_read() will wait for a newly produced sample.
   */
  wio_delay_ms(80U);
  if (!read_registers(device->address, LIS3DH_REG_OUT_X_L, discarded, sizeof(discarded))) {
    return false;
  }
  device->initialized = true;
  return true;
}

static int16_t axis_mg(uint8_t low, uint8_t high) {
  int32_t value = (int32_t)((uint32_t)low | ((uint32_t)high << 8));
  if (value >= 32768) {
    value -= 65536;
  }
  /* High-resolution output is left-justified 12-bit two's-complement.
   * At +/-2 g its nominal sensitivity is 1 mg/digit. Division avoids relying
   * on implementation-defined right shifts of negative signed integers.
   */
  return (int16_t)(value / 16);
}

bool lis3dh_read(lis3dh_t *device, lis3dh_sample_t *sample) {
  uint8_t status = 0U;
  uint8_t data[6];
  uint32_t start;
  lis3dh_sample_t result;

  if ((device == NULL) || (sample == NULL) || !device->initialized ||
      (device->who_am_i != LIS3DH_WHO_AM_I_VALUE) ||
      ((device->address != 0x18U) && (device->address != 0x19U))) {
    return false;
  }
  start = wio_millis();
  /* The finite retry count also bounds this loop if the caller lost SysTick. */
  for (uint32_t attempt = 0U; attempt < LIS3DH_DATA_TIMEOUT_MS; ++attempt) {
    if (!read_registers(device->address, LIS3DH_REG_STATUS, &status, 1U)) {
      return false;
    }
    if ((status & LIS3DH_STATUS_XYZ_READY) != 0U) {
      break;
    }
    if ((uint32_t)(wio_millis() - start) >= LIS3DH_DATA_TIMEOUT_MS) {
      return false;
    }
    wio_delay_ms(1U);
  }
  if (((status & LIS3DH_STATUS_XYZ_READY) == 0U) ||
      !read_registers(device->address, LIS3DH_REG_OUT_X_L, data, sizeof(data))) {
    return false;
  }
  result.x_mg = axis_mg(data[0], data[1]);
  result.y_mg = axis_mg(data[2], data[3]);
  result.z_mg = axis_mg(data[4], data[5]);
  result.status = status;
  *sample = result;
  return true;
}
