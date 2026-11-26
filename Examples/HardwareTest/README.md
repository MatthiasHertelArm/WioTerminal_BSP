# Wio Terminal hardware test

This example provides bounded firmware smoke checks and an interactive board test, with results in RAM
instead of serial logging. A `PASS` means only the specific check described below succeeded; it does not
certify a complete peripheral, the board, or CMSIS-Driver conformance.

The final version-3 image was run on the attached board: 13 automated checks passed, the radio handshake
timed out, four checks remain manual and seven were skipped. The SD card did not respond, so filesystem
hardware validation remains outstanding. The [validation record](#recorded-validation--2026-10-28)
separates actual target observations from implemented checks and fixture-dependent work.

## Build, clocks and boot-loader safety

In this checkout, the [solution](HardwareTest.csolution.yml) builds the local BSP (`path: ../..`)
with GCC. Pack generation removes that override from the packaged copy, so an example copied into another
workspace resolves the installed BSP. The [project](HardwareTest.cproject.yml) selects the CMSIS I2C/SPI/USART/read-only Flash drivers
and the Core, Sensors, Display, Storage and Network SDK components. See the [SDK guide](../../Documents/SDK.md) for their
interfaces, pin assignments, dependencies and ownership rules.

The documentation links above and below are relative to the source/installed pack tree. If only the example
folder was copied, open the SDK guide and overview from the installed pack's `Documents` folder instead.

The [linker configuration](RTE/GCC/Device/ATSAMD51P19A/samd51p19a_flash.ld) starts the application at
`0x00004000`, retaining the first 16 KB for the UF2 boot loader. Keep [pyocd_user.py](pyocd_user.py) beside
the solution: it selects the board's corrected 8 KB-sector internal-flash algorithm instead of the
affected Microchip DFP algorithm. Use the CMSIS Solution build/load/debug actions with a CMSIS-DAP probe
on the SWD pads; see the [BSP debug notes](../../Documents/OVERVIEW.md#debugging-and-known-limitations-of-the-dfp).
Do not use a mass erase to work around a connection failure. Loading this application writes the MCU's
application region; the runtime storage tests below are strictly read-only.

The SDK requires the supported nominal 48 MHz DFLL/GCLK0 and CPU-divider configuration. It verifies this
configuration instead of changing the CPU clock, and owns SysTick (1 ms) and DWT CYCCNT. Do not combine
this example unchanged with another SysTick handler, clock setup, DWT counter owner or RTOS timebase.
External header/Grove power controls, USB host power and radio enable are not switched on by the test;
their existing control-pin states are left alone. The SDK starts the buzzer and infrared outputs low.

## Reading the version-3 results

Inspect `g_hw_test`, declared in [HardwareTest.h](HardwareTest.h), using the debugger. A breakpoint on
`hardware_test_checkpoint` in [HardwareTest.c](HardwareTest.c) provides a stable observation point; the
firmware does not halt itself. The structure has magic `0x57494F54` (`WIOT`) and
`version = HARDWARE_TEST_REPORT_VERSION = 3`.

| Result value | Meaning |
|:-------------|:--------|
| `HW_NOT_RUN = 0` | No recorded result, including incomplete prerequisite-qualified argument coverage |
| `HW_PASS = 1` | The narrowly defined check succeeded |
| `HW_FAIL = 2` | An attempted check or its required initialization failed |
| `HW_MANUAL = 3` | Physical observation or interaction is still required |
| `HW_SKIPPED = 4` | Not validated: deliberately excluded or a documented media/firmware/fixture prerequisite was unavailable |

`completed = 1` means the startup suite reached its end, not that every check passed. Review all fields
under `checks` and the `passed`, `failed`, `manual`, `skipped` and `not_run` summary counts. Early core
initialization failure leaves `completed = 0`, reports clock failure and sets `phase = 0xE001`.

Normal phases are 1 Core, 2 driver API/ownership, 3 GPIO, 4 accelerometer, 5 ADC, 6 LCD, 7 Flash,
8 SD/FAT, 9 radio probe and 10 interactive.
`heartbeat` advances in the interactive loop after the startup suite. Event masks and completion counters
are diagnostics, not independent pass criteria: an I2C TRANSFER_DONE event can also report an incomplete
transfer or another error.

## Automated smoke checks and their limits

| Check | What a PASS establishes | What it does not establish |
|:------|:------------------------|:---------------------------|
| `clock` | Supported clock configuration and consistency between a CPU-cycle-derived delay and the millisecond counter | Absolute oscillator frequency or accuracy against an independent clock |
| `bootloader_protection` | The observed BOOTPROT setting is 13, corresponding to the protected 16 KB boot-loader region | Boot-loader contents, integrity or successful boot-loader operation |
| `driver_api` | Expected API version values, a small set of invalid-argument return codes, and test callback registration | Full CMSIS conformance, all capabilities/error paths, external pin timing or UART loopback |
| `sercom0_ownership` | USART0 and SPI0 reject simultaneous ownership in both directions; powering off the inactive competitor does not release the owner | External radio communication or arbitrary concurrent lifecycle calls |
| `led_gpio` | The PORT output latch changes high and low through VIO | LED light output, wiring continuity or optical brightness |
| `accelerometer_id` | The onboard sensor reports the expected LIS3DH WHO_AM_I value `0x33` | Motion response, mounting orientation, scale or offset accuracy |
| `accelerometer_samples` | Initialization succeeds and eight fresh XYZ reads complete | Calibration or a known acceleration stimulus; values and squared norm are diagnostic only, with no arbitrary physical thresholds |
| `light_adc`, `microphone_adc` | ADC initialization and 32 conversions for each input complete; min/max/mean are recorded | Response to changing light/sound, sensor health, lux/dB accuracy or audio capture |
| `lcd_spi` | Initialization, color-bar writes and diagnostic-ID transfer complete on the host SPI path with completion events | A connected/working panel, a correct visible image or a valid controller ID; zero/all-ones ID is not an identification PASS |
| `sdk_arguments` | All three prerequisite-qualified argument-check groups ran and rejected their selected invalid inputs | Exhaustive input/error-path testing; an uninitialized interface cannot earn a PASS by rejecting everything |
| `flash_id` | JEDEC identity matches supported `EF 40 16` or `EF 70 16` | Full capacity, stored-data integrity or write/erase operation |
| `flash_read` | Two reads of 16 bytes at offset zero complete and match; a sample hash is recorded | Correctness of those bytes against a reference, all-address coverage, capacity or endurance |
| `sd_card` | SD initialization/CSD parsing and CRC-checked sector-zero read succeed with the software write gate closed | Whole-card integrity, filesystem support or writes; initialization NOT_READY/TIMEOUT is SKIPPED with its result code retained, but subsequent read/cleanup failures are FAIL |
| `filesystem` | Read-only FAT mount, up to 64 root-directory entries and clean close/unmount succeed | All files, directory completeness when capped, exFAT, writes or crash consistency |
| `filesystem_fixture` | A nonempty `0:/WIOTEST.TXT` supplied by the user is opened/read/closed; count and FNV-1a hash of at most 512 bytes are retained | Comparison with an independently known expected payload, whole-file coverage or writing |
| `radio` | Firmware version 2.1.3 and echo are received through the real eRPC UART | Wi-Fi association, DHCP/DNS, TCP/UDP, RF performance or BLE |

The external-Flash checks never call program or erase, including to test unsupported return codes.
`Driver_Flash0` uses read-only single-lane GPIO SPI on the QSPI pads, not memory-mapped QSPI/XIP. No card
or external-flash contents are intentionally modified.

### Storage and radio prerequisites

Insert an existing FAT12/16/32 microSD before starting the test. The detect pin's raw level is recorded,
but initialization is attempted independently of an assumed switch polarity. Missing/unresponsive media
and unsupported filesystems are not PASS results. An exFAT card may pass the block test but is not supported
by this filesystem profile; do not reformat existing media just for the test. To cover file reads, place a
small, nonempty `WIOTEST.TXT` in the root directory using another computer. The test never creates it,
opens unrelated user files or retains filenames/payloads. Inspect `sd_result`, sector geometry,
`fs_result`, `fs_fixture_result`, `fs_unmount_result`, entry count and hashes alongside the checks.
The three `FRESULT` fields are `HARDWARE_TEST_RESULT_UNSET` (`0xFFFFFFFF`) until that operation is attempted.

The radio probe attaches to USART0 (PB24 TX, PC24 RX, SERCOM0, 614400 8N1), not the module's separate
programming/log UART. It leaves CHIP_PU and boot pins untouched and issues only version/echo RPCs.
The radio must already be running with a clean stream. Incompatible firmware is SKIPPED; a failed
transport/handshake is FAIL with `network_result`, state, remote status and counters retained. That failure
does not by itself establish defective hardware. No automatic reset, firmware update or recovery is attempted.
Saved firmware profiles can auto-reconnect independently; a successful explicit join can write credentials
to radio flash, so the test never joins a network. See [network contracts](../../Documents/SDK.md#rtl8720dn-networking).

### Argument-check accounting

Each group runs immediately after successful initialization of its own interface, not after an unrelated
device test. `sdk_arguments_run` and `sdk_arguments_failed` use the following bits from
[HardwareTest.h](HardwareTest.h); `HW_ARGUMENT_ALL` is `0x07`:

| Mask | Bit | Selected invalid cases |
|:-----|:----|:-----------------------|
| `HW_ARGUMENT_I2C` | 0 (`0x01`) | Invalid seven-bit address on the initialized internal I2C interface |
| `HW_ARGUMENT_ADC` | 1 (`0x02`) | Unsupported ADC input on the initialized ADC interface |
| `HW_ARGUMENT_LCD` | 2 (`0x04`) | Out-of-bounds/empty rectangle, NULL pixel data and NULL ID destination on the initialized LCD interface |

The aggregate is `HW_FAIL` if any executed group fails, `HW_PASS` only when all three run and pass, and
`HW_NOT_RUN` otherwise. Missing prerequisites are not silently converted into passing argument tests.

## Interactive checks

After the startup suite, the LED toggles every 500 ms and the LCD should show four vertical bars, from
left to right: red, green, blue and white. Confirm these physically. `led_visual` and `lcd_visual` remain
manual results; GPIO latch readback and SPI completion cannot substitute for visual confirmation.

For buttons A/B/C and each of the five joystick actions, first release the input, then press it, then
release it again. Each level must remain stable long enough to pass the software debounce. The button
fields use CMSIS VIO masks (`vioBUTTON0..2` and `vioJOY*`), with active meaning pressed:

| Field | Meaning |
|:------|:--------|
| `buttons` | Current debounced active levels |
| `buttons_seen` | Any debounced active levels observed; diagnostic only, not proof of a working switch |
| `buttons_released` | Inputs for which a debounced released baseline has been observed |
| `buttons_pressed` | Inputs pressed after their released baseline |
| `buttons_completed` | Inputs released again after that qualified press |

Only completion of the release–press–release sequence for every button and joystick action changes
`checks.buttons` from manual to PASS. An input stuck active from startup cannot pass by itself.

Button A is armed for the buzzer only after a released baseline has been observed. A subsequent press
requests an 80 ms, 2 kHz tone and increments `buzzer_requests` when the tone helper succeeds. Holding A
at startup does not request a tone. The request counter proves only execution of the GPIO-tone routine;
audibility and sound quality remain manual, and `checks.buzzer` is not automatically promoted to PASS.

Record LED/LCD appearance, switch actions and buzzer audibility alongside any captured RAM results.
Do not write PASS into the result structure merely to make the summary appear successful.

## Intentionally skipped coverage

| Check | Default behavior / additional work needed |
|:------|:------------------------------------------|
| `external_interfaces` | Header/Grove transports are not driven; external I2C/SPI devices and UART loopback need appropriate fixtures and an explicit test |
| `network_link` | No Wi-Fi join, DHCP/DNS or TCP/UDP test; needs an explicitly authorized network and endpoint. The version/echo probe is reported separately under `radio`. |
| `usb` | No USB device/host stack or enumeration test; host power is not enabled |
| `infrared` | Output starts low; no modulation or external optical measurement |

These checks stay `HW_SKIPPED`, not PASS. Card/filesystem/fixture prerequisites may add more skipped
checks. Board capabilities listed in the BSP are not a claim that this example implements or validates
every peripheral. Physical and fixture-dependent results need their own evidence.

## Recorded validation — 2026-10-28

- The final version-3 GCC 14.3.1 Debug build succeeded through CMSIS Solution and was programmed/debugged
  through its load-and-debug action with the supplied safe flash-algorithm hook. The final build includes
  the SD timeout handling and FAT geometry/failed-FSInfo-read guards covered by the host regressions.
- Build-artefact inspection: first nonempty LOAD segment at `0x00004000`; initialized RAM data is loaded
  from flash at `0x0000F77C`. No nonempty LOAD segment covers the boot loader or fuse region. ROM use is
  48,932 bytes; the HSRAM allocation is 26,216 bytes, including the 4 KiB stack and 512-byte linker heap
  reservation. The SDK itself does not allocate from the heap. These are this example's linked sizes;
  unused SDK APIs are discarded, so they are not a whole-SDK footprint claim.
- The final checkpoint reported `version=3`, `completed=1`, `phase=10`, `elapsed_ms=5835` and
  **13 PASS, 1 FAIL, 4 MANUAL, 7 SKIPPED, 0 NOT_RUN**. Clock, boot-loader protection, driver API,
  SERCOM0 ownership, LED output latch, LIS3DH identity/eight samples, light/microphone ADC, LCD SPI,
  argument checks, external-flash identity and repeated reads passed. LED/LCD appearance, buttons and
  buzzer remain manual; transport completion is not physical observation.
- `radio` failed with `network_result=-4` (`WIO_NETWORK_TIMEOUT`). The version request completed a
  12-byte host UART transmission, but no receive bytes or firmware version arrived. The host entered
  its faulted state; no Wi-Fi join, DHCP/DNS or socket operation was attempted. SERCOM0 pin routing,
  receive-pad selection and retained baud configuration were checked against the implementation:
  `BAUD=0xCB92` gives approximately 614410 baud at the verified nominal 48 MHz clock. CHIP_PU/PA18 read
  high after temporarily enabling only its input sampler, which was then restored. No enable, reset,
  boot or power pin was driven. These checks do not establish compatible/running radio firmware, a
  clean eRPC stream or successful remote reception; the timeout's cause remains unresolved.
- `sd_card` was skipped after `sd_result=4` (`WIO_SD_TIMEOUT`), with zero reported sectors. Raw card
  detect was 1, which alone does not prove presence or absence. `filesystem` and `filesystem_fixture`
  were consequently skipped; `fs_result=0xFFFFFFFF` records that mounting was not attempted. No card
  write, format or erase was performed. The other skipped checks were external interfaces, network
  link, USB and infrared.
- BOOTPROT was 13. All 16,384 boot-loader bytes matched the pre-programming capture exactly after both
  programming passes, with unchanged FNV-1a32 `0x262235A7`. Temporary breakpoints were removed and the
  board was left running the interactive loop.
- All five [native regression suites](../../Tests/README.md) passed under AddressSanitizer and
  UndefinedBehaviorSanitizer: SERCOM0 ownership, SD transport, FatFs, network protocol and network UART.
  The storage fixtures cover FAT12/16/32, read/write round trips, malformed geometry, failed media and
  open-object protection in host memory; this is not a physical-card write validation.
- Pack/schema validation with `packchk` 1.4.5 reported zero errors and zero warnings against
  `Microchip::SAMD51_DFP@3.9.276` and `ARM::CMSIS@6.3.0`. XML, packaging-shell syntax and diff checks
  passed. [CI](../../.github/workflows/build.yml) is configured for source-tree and copied installed-pack
  examples plus sanitizer-enabled native tests; neither remote CI nor a fresh generated-pack install
  was run during this middleware validation.

Earlier connection attempts reported AP2–AP4 FAULT ACK discovery errors and failed with
`PC register is not available`; those attempts did not program an image. A subsequent connection after
the extension/window reload allowed inspection of the already-running version-2 baseline (12 PASS,
0 FAIL, 4 MANUAL, 5 SKIPPED). AP discovery warnings were still emitted during the successful version-3
sessions. Their cause was not established; successful later access is not evidence of a firmware fix
for the earlier debugger failure.

For hardware validation, inspect BOOTPROT before programming,
load through CMSIS Solution with the supplied hook, then capture `g_hw_test` at the checkpoint. Verify the
physical display/LED and input/buzzer behavior separately. Completing storage coverage needs responsive
FAT media and the optional read fixture. Radio recovery/reset needs explicit authorization because saved
profiles may reconnect independently; end-to-end network tests also need an authorized network and
endpoint. Do not bypass a connection problem with a mass erase or by replacing the protected boot loader.
