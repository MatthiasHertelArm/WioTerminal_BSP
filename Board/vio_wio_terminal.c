/******************************************************************************
 * @file     vio_wio_terminal.c
 * @brief    Virtual I/O implementation for the Seeed Studio Wio Terminal
 * @version  V1.0.0
 * @date     28. October 2026
 ******************************************************************************/
/*
 * Copyright (c) 2026 Arm Limited (or its affiliates).
 * All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*! \page vio_wio_terminal Physical I/O Mapping

The table below lists the physical I/O mapping of this CMSIS-Driver VIO implementation.

| Virtual I/O   | Variable       | Board component            | Pin
|:--------------|:---------------|:---------------------------|:---------------------
| vioBUTTON0    | vioSignalIn.0  | Button A (WIO_KEY_A)       | PC26 (active low)
| vioBUTTON1    | vioSignalIn.1  | Button B (WIO_KEY_B)       | PC27 (active low)
| vioBUTTON2    | vioSignalIn.2  | Button C (WIO_KEY_C)       | PC28 (active low)
| vioJOYup      | vioSignalIn.4  | 5-way switch up            | PD20 (active low)
| vioJOYdown    | vioSignalIn.5  | 5-way switch down          | PD08 (active low)
| vioJOYleft    | vioSignalIn.6  | 5-way switch left          | PD12 (active low)
| vioJOYright   | vioSignalIn.7  | 5-way switch right         | PD09 (active low)
| vioJOYselect  | vioSignalIn.8  | 5-way switch press         | PD10 (active low)
| vioLED0       | vioSignalOut.0 | Blue user LED              | PA15 (active high)

The button and switch inputs use the internal pull-up resistors of the ATSAMD51.
*/

#include <string.h>
#include "cmsis_vio.h"

#include "RTE_Components.h"                 // Component selection
#include CMSIS_device_header

// VIO input, output definitions
#ifndef VIO_VALUE_NUM
#define VIO_VALUE_NUM           5U          // Number of values
#endif

// VIO input, output variables
__USED uint32_t vioSignalIn;                // Memory for incoming signal
__USED uint32_t vioSignalOut;               // Memory for outgoing signal
__USED int32_t  vioValue[VIO_VALUE_NUM];    // Memory for value used in vioGetValue/vioSetValue

// PORT groups
#define PORT_A                  0U
#define PORT_B                  1U
#define PORT_C                  2U
#define PORT_D                  3U

// GPIO pin descriptor
typedef struct {
  uint8_t  group;                           // PORT group (PORT_A..PORT_D)
  uint8_t  pin;                             // Pin number in the group
  uint32_t signal;                          // VIO signal mask
} vioPin_t;

// User LED (active high) mapped to vioLED0
static const vioPin_t ledPins[] = {
  { PORT_A, 15U, vioLED0 }                  // Blue user LED
};

// Buttons and 5-way switch (active low, internal pull-up)
static const vioPin_t btnPins[] = {
  { PORT_C, 26U, vioBUTTON0   },            // Button A
  { PORT_C, 27U, vioBUTTON1   },            // Button B
  { PORT_C, 28U, vioBUTTON2   },            // Button C
  { PORT_D, 20U, vioJOYup     },            // 5-way switch up
  { PORT_D,  8U, vioJOYdown   },            // 5-way switch down
  { PORT_D, 12U, vioJOYleft   },            // 5-way switch left
  { PORT_D,  9U, vioJOYright  },            // 5-way switch right
  { PORT_D, 10U, vioJOYselect }             // 5-way switch press
};

#define VIO_LED_NUM     (sizeof(ledPins) / sizeof(ledPins[0]))
#define VIO_BUTTON_NUM  (sizeof(btnPins) / sizeof(btnPins[0]))

// Configure a pin as push-pull output with the given initial level
static void pinOutput (const vioPin_t *p, uint32_t level) {
  port_group_registers_t *port = &PORT_REGS->GROUP[p->group];

  if (level != 0U) {
    port->PORT_OUTSET = (1UL << p->pin);
  } else {
    port->PORT_OUTCLR = (1UL << p->pin);
  }
  port->PORT_PINCFG[p->pin] = 0U;
  port->PORT_DIRSET = (1UL << p->pin);
}

// Configure a pin as input with pull-up
static void pinInputPullUp (const vioPin_t *p) {
  port_group_registers_t *port = &PORT_REGS->GROUP[p->group];

  port->PORT_DIRCLR = (1UL << p->pin);
  port->PORT_OUTSET = (1UL << p->pin);      // OUT = 1 selects the pull-up
  port->PORT_PINCFG[p->pin] = PORT_PINCFG_INEN_Msk | PORT_PINCFG_PULLEN_Msk;
}

// Initialize test input, output.
void vioInit (void) {
  uint32_t n;

  vioSignalIn  = 0U;
  vioSignalOut = 0U;

  memset(vioValue, 0, sizeof(vioValue));

  // LED off
  for (n = 0U; n < VIO_LED_NUM; n++) {
    pinOutput(&ledPins[n], 0U);
  }

  // Buttons and 5-way switch
  for (n = 0U; n < VIO_BUTTON_NUM; n++) {
    pinInputPullUp(&btnPins[n]);
  }
}

// Set signal output.
void vioSetSignal (uint32_t mask, uint32_t signal) {
  port_group_registers_t *port;
  uint32_t n;

  vioSignalOut &= ~mask;
  vioSignalOut |=  mask & signal;

  for (n = 0U; n < VIO_LED_NUM; n++) {
    if ((mask & ledPins[n].signal) != 0U) {
      port = &PORT_REGS->GROUP[ledPins[n].group];
      // LED on: pin high; LED off: pin low
      if ((signal & ledPins[n].signal) != 0U) {
        port->PORT_OUTSET = (1UL << ledPins[n].pin);
      } else {
        port->PORT_OUTCLR = (1UL << ledPins[n].pin);
      }
    }
  }
}

// Get signal input.
uint32_t vioGetSignal (uint32_t mask) {
  uint32_t signal;
  uint32_t n;

  for (n = 0U; n < VIO_BUTTON_NUM; n++) {
    if ((mask & btnPins[n].signal) != 0U) {
      // Button pressed = pin low
      if ((PORT_REGS->GROUP[btnPins[n].group].PORT_IN & (1UL << btnPins[n].pin)) == 0U) {
        vioSignalIn |=  btnPins[n].signal;
      } else {
        vioSignalIn &= ~btnPins[n].signal;
      }
    }
  }

  signal = vioSignalIn & mask;

  return signal;
}

// Set value output.
void vioSetValue (uint32_t id, int32_t value) {
  uint32_t index = id;

  if (index >= VIO_VALUE_NUM) {
    return;                             /* return in case of out-of-range index */
  }

  vioValue[index] = value;
}

// Get value input.
int32_t vioGetValue (uint32_t id) {
  uint32_t index = id;
  int32_t  value = 0;

  if (index >= VIO_VALUE_NUM) {
    return value;                       /* return default in case of out-of-range index */
  }

  value = vioValue[index];

  return value;
}
