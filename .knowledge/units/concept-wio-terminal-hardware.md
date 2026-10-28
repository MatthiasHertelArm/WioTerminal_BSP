---
id: concept-wio-terminal-hardware
type: concept
title: "Seeed Studio Wio Terminal hardware: parts, debug access and pin map"
summary: "ATSAMD51P19A with ILI9341 LCD on SERCOM7, RTL8720DN Wi-Fi/BLE, LIS3DH on SERCOM4, 4 MB QSPI Flash, user LED PA15, buttons PC26..28, 5-way switch on PORT D; SWD only through test pads, no on-board probe."
domain: board
status: draft
applies_to:
  board: [Wio-Terminal]
  device: [ATSAMD51P19A]
  pack: [SeeedStudio::WioTerminal_BSP]
source: Seeed-Studio/OSHW-WioTerminal BOM; Seeed-Studio/ArduinoCore-samd variants/wio_terminal/variant.cpp and variant.h; wiki.seeedstudio.com (Wio Terminal, Software-SWD); Zephyr board documentation seeed/wio_terminal; register reads on a Wio Terminal 2026-10-28
license: Apache-2.0
---

## Parts (BOM of Seeed-Studio/OSHW-WioTerminal)

| Function | Part |
|:--|:--|
| MCU | Microchip ATSAMD51P19A-CTUT (TFBGA-120) |
| Wireless | Realtek RTL8720DN-VA1-CG with its own GigaDevice GD25Q16CEIGR Flash |
| External Flash | Winbond W25Q32JVSSIQ, 4 MB, QSPI |
| Accelerometer | ST LIS3DHTR, I2C |
| Display | 2.4 inch TFT 320x240, ILI9341 controller, SPI |
| Power | Diodes AP2112K-3.3 LDO, MPS MP2161 buck |
| Others | 32.768 kHz crystal, blue user LED, buzzer, microphone with LMV358, light sensor, IR emitter, microSD holder, USB Type-C |

## Debug access

- No on-board probe. SWD test pads SWCLK, SWDIO, RESET, 3V3 and GND on the front side of the PCB (casing removed;
  Seeed lists SWCLK, SWDIO and GND, Zephyr also RESET and 3V3). Tested with a ULINKplus at 2 MHz.
- The DAP has two AHB-APs (AP#0 system bus with ROM table at 0x41003000, AP#1); pyOCD logs
  `Error probing AP#2..#4 ... FAULT ACK` at every connect, which is harmless.
- DSU external registers: CTRL 0x41002100, STATUSA 0x41002101 (CRSTEXT bit 1), STATUSB 0x41002102 (PROT bit 0,
  DBGPRES bit 1, HPE bit 4); read 0x00, 0x00, 0x12 on the running board.

## Pin map (Seeed Arduino core)

| Function | Pins |
|:--|:--|
| User LED (blue) | PA15, active high |
| Buttons A, B, C | PC26, PC27, PC28, active low, internal pull-up |
| 5-way switch | up PD20, down PD08, left PD12, right PD09, press PD10, active low, internal pull-up |
| Buzzer | PD11 (TCC0 WO[4]) |
| Light sensor / microphone | PD01 (ADC1 AIN15) / PC30 (ADC1 AIN12) |
| IR emitter | PB31 (TC0 WO[1]) |
| LCD (SERCOM7, function D) | CS PB21, SCK PB20, MISO PB18, MOSI PB19, D/C PC06, RESET PC07, backlight PC05 |
| microSD (SERCOM6, function C) | MOSI PC16, SCK PC17, MISO PC18, CS PC19, detect PD21 |
| LIS3DH (SERCOM4, function D) | SDA PA13, SCL PA12, INT1 PC21 |
| Grove I2C (SERCOM3, function D) | SDA PA17, SCL PA16 |
| Header UART (SERCOM2, function C) | TX PB26, RX PB27 |
| RTL8720DN | UART SERCOM1 TX PC22 / RX PC23, SPI SERCOM0, CHIP_PU PA18 |
| QSPI Flash | IO0..3 PA08..PA11, SCK PB10, CS PB11 |
| SWD | SWCLK PA30, SWDIO PA31, SWO PB30 |

`Board/Include/wio_terminal.h` of the pack has the complete map as `WIO_PIN_*`. Verified with the debugger:
PA15 output toggles, the button and switch inputs read high when released (PORT C IN 0x1C000000, PORT D IN
0x00101700 with pull-ups); pressed levels and the LED itself were not observed.

## Clocks

After reset (and when the boot loader starts the application) the core runs at 48 MHz from the DFLL; the DFP
`SystemCoreClock` is 48 MHz. 120 MHz needs the DPLL set up by the application.
