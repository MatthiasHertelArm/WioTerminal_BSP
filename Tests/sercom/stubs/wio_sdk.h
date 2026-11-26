/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TEST_WIO_SDK_H_
#define TEST_WIO_SDK_H_
#include <stdint.h>
typedef int32_t IRQn_Type;
enum { SERCOM0_0_IRQn = 46 };
uint32_t __get_PRIMASK(void);
void __disable_irq(void);
void __set_PRIMASK(uint32_t value);
void __DMB(void);
void NVIC_DisableIRQ(IRQn_Type irq);
void NVIC_ClearPendingIRQ(IRQn_Type irq);
#endif
