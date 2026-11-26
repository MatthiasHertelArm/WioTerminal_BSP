/* SPDX-License-Identifier: Apache-2.0 */
/* Onboard LIS3DH, using the internal CMSIS-Driver I2C4 bus only. */
#ifndef WIO_LIS3DH_H_
#define WIO_LIS3DH_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LIS3DH_WHO_AM_I_VALUE 0x33U

typedef struct {
  uint8_t address;
  uint8_t who_am_i;
  bool initialized;
} lis3dh_t;

typedef struct {
  int16_t x_mg;
  int16_t y_mg;
  int16_t z_mg;
  uint8_t status;            /* Raw STATUS_REG, including data-overrun flags. */
} lis3dh_sample_t;

/* Requires wio_sdk_init(). Probe initializes only the internal bus and reads
 * WHO_AM_I at the two legal LIS3DH addresses (0x18, 0x19); it does not scan
 * Grove or change sensor configuration. Success leaves initialized=false.
 */
bool lis3dh_probe(lis3dh_t *device);

/* Probe and configure 100 Hz, +/-2 g, high-resolution, little-endian output,
 * block-data-update, FIFO bypass and no sensor interrupt outputs.
 * Includes an 80 ms settling delay. Uses the fixed, onboard internal bus.
 */
bool lis3dh_init(lis3dh_t *device);

/* Wait for fresh XYZ data (bounded) and return nominal milligravity values
 * in the sensor's native axes. No board/display-coordinate remapping or offset
 * calibration is applied. Slow callers may observe overrun flags: this means
 * samples were missed, not that the returned current sample is invalid.
 * Leaves *sample unchanged on failure. These APIs are not reentrant.
 */
bool lis3dh_read(lis3dh_t *device, lis3dh_sample_t *sample);

#ifdef __cplusplus
}
#endif
#endif
