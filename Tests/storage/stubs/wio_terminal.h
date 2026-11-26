/* SPDX-License-Identifier: Apache-2.0 */
#ifndef STORAGE_TEST_WIO_TERMINAL_H_
#define STORAGE_TEST_WIO_TERMINAL_H_
#include <stdint.h>

extern uint32_t storage_test_ipsr;
extern uint32_t storage_test_primask;
extern uint32_t storage_test_basepri;
extern uint32_t storage_test_faultmask;
static inline uint32_t __get_IPSR(void) { return storage_test_ipsr; }
static inline uint32_t __get_PRIMASK(void) { return storage_test_primask; }
static inline uint32_t __get_BASEPRI(void) { return storage_test_basepri; }
static inline uint32_t __get_FAULTMASK(void) { return storage_test_faultmask; }

#define WIO_PIN_SD_CS 83U
void wio_gpio_output(uint8_t pin, uint32_t level);
void wio_gpio_write(uint8_t pin, uint32_t level);
#endif
