# WioTerminal_BSP

The **Seeed Studio Wio Terminal Board Support Pack (BSP)**:

- Describes the board (mounted Microchip ATSAMD51P19A, SWD test pads, on-board parts, features, documentation)
  so that it can be selected as `SeeedStudio::Wio-Terminal` in csolution projects.
- Provides a flash programming algorithm for the internal Flash with 8 KB sectors, so that downloads leave the
  UF2 boot loader in the first 16 KB untouched.
- Provides a CMSIS-Driver VIO implementation for the user LED, the buttons A, B, C and the 5-way switch.
- Provides the pin map of the on-board peripherals (`wio_terminal.h`) with GPIO helpers.
- Provides interrupt-driven CMSIS I2C, SPI and USART drivers, a read-only external-Flash driver, and
  bare-metal Core, Sensors, Display, Storage and Network SDK components; see the [SDK guide](SDK.md) for contracts and limits.
- Contains the Blinky example in *csolution format* for the [CMSIS-Toolbox](https://open-cmsis-pack.github.io/cmsis-toolbox/)
  and the VS Code [Arm CMSIS Solution](https://marketplace.visualstudio.com/items?itemName=Arm.cmsis-csolution)
  extension, linked behind the boot loader, for Arm Compiler 6 and GCC.
- Contains [HardwareTest](../Examples/HardwareTest/README.md), a GCC local-source-pack example with
  non-destructive board checks and debugger-readable results, with a per-check validation record.
- Contains knowledge units for the CMSIS Developer Assistant in `.knowledge/`.
- Requires the Device Family Pack `Microchip::SAMD51_DFP` (device header, startup and system files, SVD, flash
  algorithms) and `ARM::CMSIS`.

## Content

| Directory / File       | Content
|:-----------------------|:---------------------------------------------------------------
| [Board/](../Board/) | CMSIS-Driver VIO, [bus/Flash drivers](../Board/Drivers/) and [board SDK](../Board/SDK/)
| [Board/Include/](../Board/Include/) | Pin map, GPIO helpers, driver instances and SDK interfaces
| [Middleware/FatFs/](../Middleware/FatFs/) | Patched FatFs R0.16, fixed board configuration, license and source provenance
| [Flash/](../Flash/) | `SAMD51_512_8KB.FLM`: internal-flash algorithm of the DFP with 8 KB sectors
| [Examples/Blinky/](../Examples/Blinky/) | VIO example, boot-loader-offset linker files and debug configuration (GCC and AC6)
| [Examples/HardwareTest/](../Examples/HardwareTest/) | Board/SDK test example, boot-loader-offset linker file and debug configuration (GCC)
| [Documents/](./) | This overview, [SDK guide](SDK.md) and [DFP proposal](SAMD51_DFP_Proposal.md)
| [.knowledge/](../.knowledge/) | Knowledge units (troubleshooting, concepts, procedures) for the CMSIS Developer Assistant

## Software components

| Component                           | Description
|:------------------------------------|:---------------------------------------------------------------
| `CMSIS Driver:VIO:Board`            | Virtual I/O: vioLED0 on the blue user LED, vioBUTTON0..2 on buttons A, B, C, vioJOYup/down/left/right/select on the 5-way switch
| `Board Support:GPIO`                | Pin map (`WIO_PIN_*`) and inline GPIO helpers (`wio_gpio_output/input/function/write/toggle/read`)
| `Board Support:SDK:Core`            | Verified 48 MHz clock configuration, SysTick/DWT timebase, delays, peripheral clocks and optional buzzer tone
| `CMSIS Driver:I2C:Wio`              | 7-bit 100/400 kHz masters: I2C4 for LIS3DH and I2C3 for Grove/header; repeated START
| `CMSIS Driver:SPI:Wio`              | 8-bit masters: SPI7 LCD, SPI6 microSD transport, SPI5 header, SPI0 radio transport
| `CMSIS Driver:USART:Wio`            | Asynchronous 8N1: USART0 radio eRPC, USART1 module loader/log, USART2 header
| `CMSIS Driver:Flash:Wio`            | Read-only W25Q32JV identity/status/data over GPIO SPI; no program, erase, native QSPI or XIP
| `Board Support:SDK:Sensors`         | LIS3DH samples, raw calibrated ADC light/microphone readings and blocking I2C helpers
| `Board Support:SDK:Display`         | ILI9341 initialization, RGB565 rectangles, backlight and diagnostic ID over SPI7
| `Board Support:SDK:Storage`         | SD SPI block layer and FatFs FAT12/16/32; read-only mount by default, explicit write opt-in, no format/trim
| `Board Support:SDK:Network`         | Seeed eRPC firmware 2.1.3 station/DHCP/DNS, IPv4 TCP client/UDP; explicit join, no radio reset

The [SDK guide](SDK.md) lists exact dependencies, driver instances/pins, buffer and interrupt ownership,
safe initialization defaults and unsupported features. Filesystem/network middleware is optional, not
automatically activated by SDK Core. No TLS, Bluetooth or USB stack, DMA or RTOS integration is supplied.

## Board

| Property       | Value
|:---------------|:---------------------------------------------------------------
| Board          | `SeeedStudio::Wio-Terminal`
| Device         | `Microchip::ATSAMD51P19A` (Cortex-M4F, 120 MHz, 512 KB Flash, 192 KB RAM), after reset 48 MHz (DFLL)
| Boot loader    | UF2 boot loader in `0x00000000`..`0x00003FFF`, protected by BOOTPROT = 13
| Application    | From `0x00004000`; the boot loader sets `SCB->VTOR` and jumps there
| Debug          | SWD test pads SWCLK, SWDIO, RESET, 3V3, GND on the PCB front side (casing removed); no on-board probe
| Programming    | CMSIS-DAP probe on the SWD pads, or UF2 drag-and-drop in boot loader mode
| External Flash | 4 MB W25Q32JV on QSPI pads; read-only `Driver_Flash0` uses zero-based offsets over GPIO SPI, not the `0x04000000` mapped window; no download algorithm

## Pin mapping

From the Seeed Studio Arduino core for the Wio Terminal (`variants/wio_terminal/variant.cpp`). Function C is
SERCOM, D is SERCOM-ALT.

| Function            | Pins
|:--------------------|:----------------------------------------------------------------
| User LED (blue)     | PA15, active high
| Buttons A, B, C     | PC26, PC27, PC28, active low (internal pull-up)
| 5-way switch        | up PD20, down PD08, left PD12, right PD09, press PD10, active low (internal pull-up)
| Buzzer              | PD11 (TCC0 WO[4])
| Light sensor        | PD01 (ADC1 AIN15)
| Microphone          | PC30 (ADC1 AIN12)
| Infrared emitter    | PB31 (TC0 WO[1])
| LCD ILI9341         | SERCOM7 (D): CS PB21, SCK PB20, MISO PB18, MOSI PB19; D/C PC06, RESET PC07, backlight PC05
| microSD             | SERCOM6 (C): MOSI PC16, SCK PC17, MISO PC18, CS PC19; card detect PD21
| LIS3DH (I2C)        | SERCOM4 (D): SDA PA13, SCL PA12; INT1 PC21
| Grove I2C           | SERCOM3 (D): SDA PA17, SCL PA16
| Header UART         | SERCOM2 (C): TX PB26, RX PB27
| Header SPI          | SERCOM5 (D): MOSI PB02, SCK PB03, MISO PB00, CS PB01
| RTL8720DN           | eRPC UART SERCOM0 (C) TX PB24, RX PC24; loader/log UART SERCOM1 (C) TX PC22, RX PC23; alternate SPI0 MOSI PB24, SCK PB25, MISO PC24, CS PC25; CHIP_PU PA18
| QSPI Flash          | IO0..IO3 PA08..PA11, SCK PB10, CS PB11
| USB                 | D- PA24, D+ PA25, host power enable PA27
| SWD                 | SWCLK PA30, SWDIO PA31, SWO PB30

## Using the pack

```yaml
solution:
  packs:
    - pack: SeeedStudio::WioTerminal_BSP
    - pack: Microchip::SAMD51_DFP
    - pack: ARM::CMSIS
  target-types:
    - type: Wio-Terminal
      board: SeeedStudio::Wio-Terminal
```

Then add the components `CMSIS:CORE`, `Device:Startup` (from the DFP) and `CMSIS Driver:VIO:Board` or
`Board Support:GPIO`. To keep the boot loader, change the ROM region of the linker configuration file of
`Device:Startup` to `0x00004000`..`0x0007FFFF` as in `Examples/Blinky/RTE/<compiler>/Device/ATSAMD51P19A/`, and
copy `Examples/Blinky/pyocd_user.py` next to the `*.csolution.yml` (see below).
For the new components, follow [SDK initialization and ownership](SDK.md#initialization-ownership-and-safe-defaults)
or start from [HardwareTest](../Examples/HardwareTest/HardwareTest.csolution.yml). The SDK does not enable
external header/Grove rails, USB host power or the radio; it leaves their existing control-pin state alone.

## Debugging and known limitations of the DFP

`Microchip::SAMD51_DFP@3.9.276` has two problems that a BSP cannot fix in a CMSIS-compliant way; both are
described, with the proposed DFP changes and their validation, in `SAMD51_DFP_Proposal.md`:

- **Flash algorithm:** `SAMD51_512.FLM` declares the whole Flash as one sector, so pyOCD always erases the block
  at `0x00000000` and fails on the protected boot loader (`flash erase sector failure (address 0x00000000;
  result code 0x1)`). This pack describes the corrected algorithm as a board algorithm, but csolution lists it
  in `*.cbuild-run.yml` after the device algorithm and pyOCD uses the first one. `Examples/Blinky/pyocd_user.py`
  makes pyOCD use the board algorithm; pyOCD loads it from the folder of the solution, the working directory of
  the CMSIS Load, Run and Erase tasks.
- **Hardware reset:** an nRESET pulse resets the SW-DP and clears DHCSR and DEMCR. pyOCD's default hardware
  reset does not reconnect, so `monitor reset halt hardware` in a debug session ends with
  `SWD/JTAG communication failure (Unexpected ACK '0')`. The debug sequences that handle it can only be part of
  the DFP (the PACK schema has no `<sequences>` for boards). In a debug session use the system reset, the
  default of the launch configuration. `pyocd load` resets with nRESET after programming and disconnects first,
  which is not affected.

The UF2 boot loader starts the application after any reset, but stays in boot loader mode when a second reset
other than power-on arrives during the 500 ms after a reset (double-tap detection of uf2-samdx1). Two debugger
resets in quick succession can therefore leave the board in the boot loader; reset once more to start the
application.

## Validation

The following is the existing Blinky baseline, not a validation claim for the newly added SDK/drivers:

`packchk` without errors and warnings against `Microchip::SAMD51_DFP@3.9.276` and `ARM::CMSIS@6.3.0`. Blinky
built with Arm Compiler 6.24 and GCC 14.3.1 (CMSIS-Toolbox 2.15), downloaded and debugged on a Wio Terminal with
a ULINKplus (CMSIS-DAP, SWD) through pyOCD 0.46.0 (Arm CMSIS Debugger 1.10.0); see
[Examples/Blinky/README.md](../Examples/Blinky/README.md).

The final version-3 HardwareTest was built with GCC 14.3.1 and run on the attached board: 13 PASS,
1 FAIL, 4 MANUAL and 7 SKIPPED. The radio handshake timed out and the SD card did not respond;
filesystem and end-to-end network hardware validation remain outstanding. The first nonempty LOAD
segment is at `0x4000`, BOOTPROT was 13, and all 16 KB of boot-loader bytes were unchanged after
programming. The board was left running. All five sanitizer-enabled native regression suites passed,
and the updated pack passed `packchk` with zero errors and warnings. See
[HardwareTest results](../Examples/HardwareTest/README.md#recorded-validation--2026-10-28) for the exact
evidence and limits. [CI](../.github/workflows/build.yml) is configured to build Blinky, HardwareTest
and a copied installed-pack HardwareTest with GCC, plus the native regression suites; remote CI and
a fresh generated-pack install remain unexecuted in this middleware-validation session.
