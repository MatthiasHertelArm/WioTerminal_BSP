/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WIO_NETWORK_TEST_SDK_H_
#define WIO_NETWORK_TEST_SDK_H_
#include <stdint.h>
extern uint32_t test_primask, test_ipsr, test_basepri, test_faultmask;
static inline uint32_t __get_PRIMASK(void) { return test_primask; }
static inline uint32_t __get_IPSR(void) { return test_ipsr; }
static inline uint32_t __get_BASEPRI(void) { return test_basepri; }
static inline uint32_t __get_FAULTMASK(void) { return test_faultmask; }
static inline void __disable_irq(void) { test_primask = 1U; }
static inline void __set_PRIMASK(uint32_t value) { test_primask = value; }
static inline void __DMB(void) { }
static inline void __NOP(void) { }
uint32_t wio_millis(void);
#endif
