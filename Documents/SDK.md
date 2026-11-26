# Wio Terminal SDK and CMSIS-Drivers

This is a small, bare-metal board SDK on top of the Microchip device headers and CMSIS-Core, not an Arduino
or ASF compatibility layer. It supplies native interrupt-driven I2C, SPI and USART transports, a deliberately
read-only external-flash interface, helpers for the onboard sensors and display, optional SD/FatFs storage,
and an RTL8720DN eRPC IPv4 client. It does not use DMA,
an RTOS, a heap, or a framebuffer.

The final version-3 [HardwareTest](../Examples/HardwareTest/README.md#recorded-validation--2026-10-28)
ran on the attached board: 13 automated checks passed, the radio handshake timed out, four checks remain
manual and seven were skipped. The SD card was unresponsive, so filesystem hardware validation remains
outstanding; no network association or socket test was attempted. All five native regression suites
passed with sanitizers. Component availability and successful host transfers alone do not establish
that every peripheral has been tested.
The older Blinky validation is recorded separately in the [overview](OVERVIEW.md#validation).

## Components

These are the component selectors in the [pack description](../SeeedStudio.WioTerminal_BSP.pdsc).
All SDK and transport components require `CMSIS:CORE`, `Device:Startup`, `Board Support:GPIO`, and the
`SeeedStudio::Wio-Terminal` board. Select only the components needed by the application.

| Selector | Provided interface | Additional requirement |
|:---------|:-------------------|:-----------------------|
| `Board Support:SDK:Core` | [wio_sdk.h](../Board/Include/wio_sdk.h): timebase, delays, peripheral clocks, optional buzzer tone | — |
| `CMSIS Driver:I2C:Wio` | `Driver_I2C3`, `Driver_I2C4`, CMSIS I2C API 2.4 | SDK Core |
| `CMSIS Driver:SPI:Wio` | `Driver_SPI0`, `Driver_SPI5`, `Driver_SPI6`, `Driver_SPI7`, CMSIS SPI API 2.3 | SDK Core |
| `CMSIS Driver:USART:Wio` | `Driver_USART0`, `Driver_USART1`, `Driver_USART2`, CMSIS USART API 2.4 | SDK Core |
| `CMSIS Driver:Flash:Wio` | [wio_flash.h](../Board/Include/wio_flash.h): read-only `Driver_Flash0`, CMSIS Flash API 2.2 | SDK Core |
| `Board Support:SDK:Sensors` | [wio_i2c.h](../Board/Include/wio_i2c.h), [wio_adc.h](../Board/Include/wio_adc.h), [lis3dh.h](../Board/Include/lis3dh.h) | SDK Core and I2C driver |
| `Board Support:SDK:Display` | [wio_lcd.h](../Board/Include/wio_lcd.h): ILI9341 initialization and RGB565 rectangles | SDK Core and SPI driver |
| `Board Support:SDK:Storage` | [wio_sd.h](../Board/Include/wio_sd.h), [wio_fs.h](../Board/Include/wio_fs.h): SD blocks and FatFs FAT12/16/32 | SDK Core and SPI driver |
| `Board Support:SDK:Network` | [wio_network.h](../Board/Include/wio_network.h): RTL8720DN station, DHCP/DNS, TCP client/UDP | SDK Core and USART driver |
| `CMSIS Driver:VIO:Board` | LED, three buttons and five-way switch through CMSIS VIO | Device Startup; independent of SDK Core |
| `Board Support:GPIO` | [wio_terminal.h](../Board/Include/wio_terminal.h): pin map and inline GPIO helpers | Device Startup |

[HardwareTest.cproject.yml](../Examples/HardwareTest/HardwareTest.cproject.yml) selects all of these
components. In this checkout, its [solution](../Examples/HardwareTest/HardwareTest.csolution.yml) uses
`path: ../..` for the source pack and GCC, so changes are built directly. Pack generation removes the local
override from the packaged example so copies resolve the installed pack. The application is linked at
`0x00004000`, after the protected 16 KB UF2 boot loader. The smaller
[Blinky](../Examples/Blinky/README.md) example remains available for both GCC and Arm Compiler 6.

## Driver instances and pins

Instances follow SERCOM numbers; selecting a component does not initialize all of its instances. Function
C is SERCOM and function D is SERCOM-ALT. The [pin-map header](../Board/Include/wio_terminal.h) is the source
for the `WIO_PIN_*` names and the remaining GPIOs.

| Interface | Connection | Pins and peripheral function |
|:----------|:-----------|:-----------------------------|
| `Driver_I2C4` | Onboard LIS3DH | SDA PA13, SCL PA12; SERCOM4 D; sensor INT1 PC21 is not used by the SDK |
| `Driver_I2C3` | Grove/header I2C | SDA PA17, SCL PA16; SERCOM3 D |
| `Driver_SPI7` | ILI9341 display | MOSI PB19, MISO PB18, SCK PB20; SERCOM7 D; CS PB21, D/C PC06, reset PC07, backlight PC05 |
| `Driver_SPI6` | microSD socket | MOSI PC16, MISO PC18, SCK PC17; SERCOM6 C; CS PC19, card detect PD21 |
| `Driver_SPI5` | 40-pin header | MOSI PB02, MISO PB00, SCK PB03; SERCOM5 D; CS PB01 |
| `Driver_SPI0` | RTL8720DN transport | MOSI PB24, MISO PC24, SCK PB25; SERCOM0 C; CS PC25 |
| `Driver_USART2` | Header pins 8 and 10 | TX PB26, RX PB27; SERCOM2 C |
| `Driver_USART0` | RTL8720DN eRPC (shares SPI0 pins) | TX PB24 pad 0, RX PC24 pad 2; SERCOM0 C; 614400 baud, 8N1 |
| `Driver_USART1` | RTL8720DN programming/log UART, **not eRPC** | TX PC22, RX PC23; SERCOM1 C |
| `Driver_Flash0` | Onboard W25Q32JV | GPIO software SPI on the QSPI pads: IO0 PA08, IO1 PA09, IO2 PA10, IO3 PA11, SCK PB10, CS PB11 |
| ADC helpers | Light and microphone | ADC1 AIN15 PD01 and AIN12 PC30 |

Include [wio_drivers.h](../Board/Include/wio_drivers.h) for I2C/SPI/USART instances and
[wio_flash.h](../Board/Include/wio_flash.h) for Flash. Only the corresponding selected component supplies
each driver implementation.

## Initialization, ownership and safe defaults

Call `wio_sdk_init()` before using SDK or transport services and check its boolean result. It verifies the
nominal 48 MHz DFLL/GCLK0 configuration and CPU divider; it does not change the application's CPU clock to
make an unsupported configuration work. It owns SysTick at 1 ms and DWT CYCCNT. Do not change those clocks,
replace its SysTick handler, reset the cycle counter, or install an RTOS timebase while the SDK is in use.
An RTOS integration requires application work and is not supplied here.

Core initialization sets only the buzzer and infrared outputs low, in addition to its timing facilities.
It does not enable or reconfigure the external header/Grove power controls, USB host power, or radio chip
enable. These controls are left at their existing state, not forcibly switched off. External buses and
radio transports remain inactive until explicitly initialized, powered and configured by the application.
No Grove scan, card initialization, radio command, or external power-up is performed automatically.

Each driver instance exclusively owns its SERCOM, associated IRQ handlers, and configured pins. The
display helper owns SPI7 and its control GPIOs; the sensor I2C helpers own whichever I2C instance they
initialize, including its 100 kHz configuration. Do not mix direct driver transactions with a helper on
the same instance, or access a peripheral through another HAL while these drivers own it. Calls must be
serialized by the application; no thread locks or bus-sharing scheduler are provided.

SPI0 and USART0 are alternate owners of the same SERCOM0 hardware. Powering the second owner returns
`ARM_DRIVER_ERROR_BUSY` without disturbing the first. The shared IRQ dispatcher is part of SDK Core.
Power off/uninitialize the current owner before switching; lifecycle calls must be externally serialized
in foreground context. The network helper exclusively owns USART0. Do not initialize it separately or
start raw SPI0 transfers while networking is attached. Storage exclusively owns SPI6; do not mix raw
SD/driver calls with a mounted filesystem.

For raw I2C/SPI/USART use, call `Initialize(callback)` (NULL is allowed), `PowerControl(ARM_POWER_FULL)`,
then the appropriate `Control(...)` configuration before transferring. USART also needs explicit
`ARM_USART_CONTROL_TX` and/or `ARM_USART_CONTROL_RX` enable. Each return code must be checked.

## Asynchronous transport contract

I2C, SPI and USART transfer calls start interrupt-driven work and return without waiting for completion.
Keep transmit buffers alive and unchanged, and receive buffers alive and writable, until completion,
a terminal error with the corresponding busy flag clear, or successful abort. A USART receive error alone
does not release the receive buffer. Callbacks run in interrupt context: keep them short, and defer
blocking SDK work to main/thread context with interrupts enabled. A NULL callback does not make the
transfer synchronous; poll status and counts with an application deadline if callbacks are not used.

Initialization, reset and register synchronization waits have finite guards. Transfers themselves have
no automatic wall-clock deadline. Applications must use `ARM_I2C_ABORT_TRANSFER`, `ARM_SPI_ABORT_TRANSFER`,
or USART's separate `ARM_USART_ABORT_SEND` / `ARM_USART_ABORT_RECEIVE` controls when their deadline expires.
Do not reuse an outstanding buffer merely because an application timeout has elapsed; check abort and
driver state. Completion counts remain available until the next operation or reinitialization.

| Driver | Supported behavior | Important limits |
|:-------|:-------------------|:-----------------|
| I2C | Master transmit/receive, 7-bit addresses, standard 100 kHz and fast 400 kHz; `xfer_pending` repeated START | No slave, 10-bit addressing, high-speed mode or bus-clear operation. External pull-ups are required; SERCOM disables its internal pulls in I2C mode. |
| SPI | Master Send/Receive/Transfer, 8-bit Motorola frames, all four clock modes, MSB/LSB first, software or unused SS, configurable fill byte | No slave, hardware SS, TI or Microwire mode. The 48 MHz input clock gives discrete rates from 93.75 kHz to 24 MHz; choose a peripheral-safe rate. |
| USART | Asynchronous 8N1, independent simultaneous Send and Receive, configurable baud, transmit/receive counts and receive error events | No synchronous Transfer, hardware flow control, modem, break, receive-timeout, IrDA or smart-card support. Unsupported framing and baud configurations return errors. |

I2C transmit counts include only acknowledged data bytes. Check incomplete/NACK/error events and the
final count, not only `busy == 0`. With `xfer_pending=true`, completion clears busy but retains ownership
of the transaction for the next repeated START; finish with a non-pending phase or explicitly abort.

SPI completion waits for the final physical transmit-complete indication and received-byte accounting,
so CS or D/C may be changed after completion. `ARM_SPI_GET_BUS_SPEED` reports the selected divider's
actual rate; the driver does not exceed the requested rate, up to its 24 MHz ceiling. For SDK-managed
CS, use `ARM_SPI_SS_MASTER_UNUSED`; software SS mode is controlled explicitly with `ARM_SPI_CONTROL_SS`.

USART send completion is reported after the final byte leaves the wire, with both SEND_COMPLETE and
TX_COMPLETE events. A send abort releases the caller's buffer but already queued hardware bytes may
still drain; `tx_busy` stays set until then. Receive errors are reported without automatically ending
the requested receive; check status/events and abort if required. Bytes arriving while RX is enabled
without a pending receive buffer are discarded and reported as overflow, not queued for a later call.

## Blocking board helpers

All helpers are intended for serialized calls in main/thread context. Check every boolean result; data
is not valid merely because a call returned. The public headers specify failure/output-buffer behavior.

| Header | Behavior |
|:-------|:---------|
| [wio_sdk.h](../Board/Include/wio_sdk.h) | Millisecond counter, bounded delays and peripheral GCLK0 routing. `wio_buzzer_tone()` is an optional blocking GPIO tone, 40–10000 Hz for 1–1000 ms; never called automatically. |
| [wio_i2c.h](../Board/Include/wio_i2c.h) | Initialize only internal bus 4 or Grove bus 3 as requested. Blocking write, read and repeated-START write/read; 100 ms transfer wait by default, with abort on failure. |
| [lis3dh.h](../Board/Include/lis3dh.h) | Probe WHO_AM_I only at the two legal onboard addresses, then optionally configure 100 Hz, ±2 g, high-resolution, block-data-update mode. Returns fresh samples in nominal mg in native sensor axes; no offset calibration or coordinate remapping. |
| [wio_adc.h](../Board/Include/wio_adc.h) | Owns ADC1; factory-calibrated, polled, single 12-bit conversions with a 1.5 MHz ADC clock and VDDANA reference. Light and microphone results are raw counts, not lux or dB SPL; this is not an audio capture driver. |
| [wio_lcd.h](../Board/Include/wio_lcd.h) | ILI9341 initialization, 320×240 RGB565 fills/rectangles, backlight and diagnostic ID. Uses SPI7 at 8 MHz for writes and 4 MHz for reads, without a framebuffer. Transport errors switch off the backlight and require reinitialization. |

LCD transfer success proves only host-side SPI completion: SPI has no acknowledgement. Confirm the image
on the physical panel. A zero/all-ones diagnostic ID is not an identification pass. RGB565 pixel buffers
are contiguous, native-endian, row-major values and are no longer used once the blocking call returns.

## Read-only external Flash

`Driver_Flash0` is separate from the pack's internal-MCU download algorithm. It uses standard single-lane,
mode-0 GPIO SPI at no more than 500 kHz on the six QSPI pins. It is not a native QSPI, memory-mapped or XIP
implementation. Do not use the native QSPI peripheral, mapped window or another owner of those pins at
the same time. The chip must already be in standard SPI/standby mode.

After SDK initialization, call `Initialize(NULL)` and `PowerControl(ARM_POWER_FULL)`.
`wio_flash_read_id()` reads raw JEDEC bytes; `ReadData(offset, buffer, length)` verifies the supported
identity on every read and returns the number of bytes read or a negative CMSIS error. The supported
IDs are EF 40 16 and EF 70 16. Offsets are zero-based within the 4 MB device, not CPU addresses such as
`0x04000000`. NULL, zero-length and out-of-range reads are rejected. Pre-existing busy or suspended chip
states return an error without waiting for or changing that operation.

Flash reads are synchronous; `event_ready=0` and no callback is generated. Prefer small chunks because
large reads block the caller. Power-off/uninitialization can cancel a read, leaving a partial destination.
`GetInfo()` describes geometry, not permission to write. `ProgramData`, `EraseSector` and `EraseChip`
always return `ARM_DRIVER_ERROR_UNSUPPORTED`. The driver never sends write-enable, status-write, reset,
wake-up, program or erase commands, so it cannot recover a chip left in another protocol/power state.

## microSD and FAT filesystem

Select `Board Support:SDK:Storage`. The [SD block layer](../Board/Include/wio_sd.h) implements SD v1/v2,
SDSC and SDHC/SDXC (CSD v1/v2) over SPI6. Initialization uses 400 kHz with the nominal 48 MHz clock,
then data uses at most 4 MHz. It checks command/data CRCs, capacity and block ranges, handles byte versus
block addressing, and bounds waits. MMC/SDUC, hot removal, DMA and automatic bus sharing are not supported.
No trim/erase command is exposed. Multi-sector writes are not atomic and are never automatically retried;
a timeout can leave earlier sectors, or even the last sector, already programmed.

The [FatFs integration](../Board/Include/wio_fs.h) uses R0.16 with both official fixes from the
[FatFs patch list](https://elm-chan.org/fsw/ff/patches.html). The bundled configuration is one volume
(`0:`), FAT12/16/32, 512-byte sectors and short 8.3 names. Existing long names can be accessed through their
short aliases. exFAT, formatting, trim, dynamic allocation and reentrancy are disabled. A large SDXC card
may therefore be supported by the block driver but not mount if it contains exFAT. Do not format it merely
to make a test pass. See [FatFs provenance](../Middleware/FatFs/PROVENANCE.md) for exact upstream inputs,
checksums, configuration, license and the small local open-object guard.

After `wio_sdk_init()`, call `wio_fs_mount(false)` for a read-only mount, then the standard FatFs
`f_open`, `f_read`, `f_readdir`, `f_lseek` and close functions. Only `wio_fs_mount(true)` opts into filesystem
writes; raw block users must separately call `wio_sd_set_writable(true)`. The software write gate is closed
on initialization/deinitialization or a media/transport failure. The card's CSD write protection is also
honored. No mount or test formats or repairs the card.

Use only `wio_fs_mount`/`wio_fs_unmount`, not direct `f_mount`/`f_unmount`, and never call raw SD APIs while
mounted. Close all file/directory objects before unmounting; open files and non-root directories return
`FR_LOCKED` without changing the mount or write gate. Root-directory handles are not tracked by upstream
FatFs and must be closed explicitly. Dirty metadata/sync errors prevent unmounting. On media failure the
wrapper will not silently reinitialize a disk under existing file handles; retain the failure and recover
explicitly. Remove the card only after a successful unmount. Writes use a fixed configured timestamp,
not an RTC; this release does not supply time synchronization or filesystem journaling.

The onboard W25Q32JV remains strictly read-only. This filesystem is on **microSD**, not QSPI NOR; no
littlefs/flash translation layer is provided.

## RTL8720DN networking

Select `Board Support:SDK:Network` and use [wio_network.h](../Board/Include/wio_network.h). This is a
fixed-buffer bare-metal client for Seeed eRPC firmware **2.1.3** with its default IPv4 lwIP ABI; it is not
the CMSIS WiFi API or an Arduino compatibility layer. The radio supplies Wi-Fi/TCP/IP. The host uses
USART0 at 614400 baud, with continuously rearmed IRQ reception into an 8 KiB ring. The module's separate
USART1 programming/log connection cannot substitute for this transport.

`wio_network_init(&info, callback, context, timeout_ms)` only attaches and checks the firmware version and
echo. It does **not** enable/reset the radio, join/scan Wi-Fi, initialize TCP/IP, update firmware or change
credentials. The radio must already be running and the stream must be clean. Its existing firmware may
independently auto-reconnect to a saved network on boot, so enabling/resetting it is deliberately left to
the application. See [protocol provenance](NETWORK_PROTOCOL.md) for the pinned primary sources, wire
format, remote ABI and restrictions.

After a successful probe, explicit calls provide:

- Station join (open or WPA2-AES-PSK), DHCP status, disconnect, and IPv4 DNS resolution.
- Four generation-checked socket handles: TCP client open/connect/send/receive/close and UDP
  open/bind/sendto/recvfrom/close. Calls expose actual transferred counts; partial sends are not retried.
- Foreground event callbacks and `wio_network_poll()` for unsolicited Wi-Fi events.

There is no TLS, certificate validation, AP mode, listen/accept, IPv6 or Bluetooth API. TCP/UDP payloads
are not encrypted by this SDK. Do not use them for sensitive application data without an appropriate
application security layer. A successful join can persist the SSID/password in the radio's internal flash;
there is no promise of ephemeral credentials. The host scrubs its serialized credential buffer after use.
No credentials are compiled into HardwareTest or committed to this repository.

Calls are serialized foreground operations with a total timeout of 1–120000 ms (`poll(0)` is nonblocking).
Callbacks must not reenter the API. Call `poll` regularly while idle; the finite ring cannot absorb events
forever. CRC, framing, UART, sequence/schema errors and outstanding-RPC timeouts fault the session instead
of guessing where an unframed byte stream resumes. A host timeout does not cancel a radio-side operation.
The fault closes the host UART only; it never pulses CHIP_PU because the radio could still be writing flash.
Reinitialization is explicit and requires an independently established clean, quiescent peer. Close sockets
and disconnect explicitly when wanted: `deinit` detaches the host without destroying remote resources.

## HardwareTest scope and exclusions

[HardwareTest](../Examples/HardwareTest/README.md) exposes results in debugger-readable RAM instead of
adding a serial logger. Its default checks cover clocks and boot-loader protection, driver argument/API
checks, LED GPIO, LIS3DH identity/samples, raw light/microphone conversions, LCD SPI transfers and
read-only flash identity/repeated reads, shared-SERCOM ownership, SD block reads/FAT enumeration and the
non-mutating radio version/echo probe. An optional user-provided `WIOTEST.TXT` fixture permits a bounded
file read without opening unrelated user files. LED appearance, LCD appearance, buttons and buzzer require
physical interaction or observation; a GPIO output readback or transfer completion is not that evidence.

No SD or QSPI contents are written by HardwareTest. Missing media, unsupported filesystems and absent
fixtures are recorded as skipped, never passed. The radio probe does not establish Wi-Fi, DNS or socket
functionality: those need an explicitly authorized network and endpoint. External interfaces, USB and
infrared are not automatically exercised. Header/Grove fixtures, a UART loopback, USB stack, infrared
protocol, touch controller, continuous audio acquisition and DMA/RTOS integration remain outside this SDK.

See the example's README for recorded hardware results and remaining manual checks; until results are
recorded, treat the new hardware validation as pending. Datasheets imported into the CMSIS documentation
cache are reference material, not additional PDFs redistributed inside this pack.
