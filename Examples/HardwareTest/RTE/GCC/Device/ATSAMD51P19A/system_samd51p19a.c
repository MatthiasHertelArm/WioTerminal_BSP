/*
 * System configuration file for ATSAMD51P19A
 *
 * Copyright (c) 2026 Microchip Technology Inc. and its subsidiaries.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdbool.h>
#include <stdint.h>
#include "samd51p19a.h"
#include "system_samd51p19a.h"

/*----------------------------------------------------------------------------
  Define clocks
 *----------------------------------------------------------------------------*/
#define SYSTEM_CLOCK    (48000000UL)

/*----------------------------------------------------------------------------
  System Core Clock Variable
 *----------------------------------------------------------------------------*/
uint32_t SystemCoreClock = (uint32_t)SYSTEM_CLOCK;  /* System Core Clock Frequency */


/*----------------------------------------------------------------------------
  System Core Clock update function
 *----------------------------------------------------------------------------*/
#if defined(__ICCARM__)
#pragma weak SystemCoreClockUpdate_PreHook
#else
__attribute__((weak))
#endif
bool SystemCoreClockUpdate_PreHook(void)
{
    // Default implementation: Do nothing.
    return false;
}

void SystemCoreClockUpdate (void)
{
    if (SystemCoreClockUpdate_PreHook()) {
        return;
    }

    SystemCoreClock = (uint32_t)SYSTEM_CLOCK;
}

/*----------------------------------------------------------------------------
  System initialization function
 *----------------------------------------------------------------------------*/
void SystemInit (void)
{
#if defined (__FPU_USED) && (__FPU_USED == 1U)
    SCB->CPACR |= ((3UL << 10U*2U) |           /* enable CP10 Full Access */
                  (3UL << 11U*2U)  );         /* enable CP11 Full Access */
#endif

#ifdef UNALIGNED_SUPPORT_DISABLE
    SCB->CCR |= SCB_CCR_UNALIGN_TRP_Msk;
#endif

    SystemCoreClock = (uint32_t)SYSTEM_CLOCK;
}
