# Seeed Studio Wio Terminal Board Support Pack

Source of the CMSIS-Pack `SeeedStudio::WioTerminal_BSP`: board support for the Seeed Studio Wio Terminal on top of
the Microchip `SAMD51_DFP`. See [Documents/OVERVIEW.md](Documents/OVERVIEW.md) for the content.

The [SDK guide](Documents/SDK.md) covers CMSIS-Driver I2C, SPI, USART and read-only external Flash, plus
bare-metal timing, sensor/display helpers, microSD/FatFs storage and RTL8720DN IPv4 networking.
[HardwareTest](Examples/HardwareTest/README.md) exercises onboard peripherals with debugger-readable results;
its validation record distinguishes measured results from pending network/media/manual checks. The
[Blinky example](Examples/Blinky/README.md) remains the minimal VIO starting point.

## Repository structure

| Directory / File                       | Content
|:---------------------------------------|:---------------------------------------------------------------
| [SeeedStudio.WioTerminal_BSP.pdsc](SeeedStudio.WioTerminal_BSP.pdsc) | Board, internal-flash algorithm, CMSIS-Drivers, SDK components and examples
| [Board/](Board/) | VIO, [transport drivers](Board/Drivers/), [SDK implementations](Board/SDK/) and [public headers](Board/Include/)
| [Middleware/FatFs/](Middleware/FatFs/) | Pinned FatFs R0.16 with both official patches, configuration, license and provenance
| [Tests/](Tests/) | Native mock-based protocol, SD/FAT and shared-peripheral regression tests
| [Flash/](Flash/) | `SAMD51_512_8KB.FLM`, the DFP internal-flash algorithm with 8 KB sectors
| [Examples/Blinky/](Examples/Blinky/) | Minimal VIO example in csolution format (GCC and Arm Compiler 6)
| [Examples/HardwareTest/](Examples/HardwareTest/) | Non-destructive board checks, debugger-readable results and local-source SDK build (GCC)
| [Documents/](Documents/) | Pack overview, SDK contracts and the changes proposed for the Microchip DFP
| [.knowledge/](.knowledge/) | Knowledge units for the CMSIS Developer Assistant knowledge base
| [.github/workflows/](.github/workflows/) | Generate/release the pack; build Blinky and HardwareTest with GCC and upload both ELF/HEX image sets
| [gen_pack.sh](gen_pack.sh) | Pack generation script ([gen-pack](https://github.com/Open-CMSIS-Pack/gen-pack) library)
| [vcpkg-configuration.json](vcpkg-configuration.json) | Tool environment (CMSIS-Toolbox, GCC, CMake, Ninja) for the Arm Environment Manager in VS Code

## Generating the pack

Prerequisites: bash 4 or later (`/opt/homebrew/bin/bash` on macOS), curl, zip (or 7z), GNU coreutils
(`brew install coreutils` on macOS), `packchk` from the
[CMSIS-Toolbox](https://github.com/Open-CMSIS-Pack/cmsis-toolbox) and `cmsis-knowledge` from
cmsis-developer-assistant-tools on the `PATH`.

```sh
./gen_pack.sh
```

The script builds `.knowledge/knowledge.db`, validates the pack description with `packchk` (against the
Microchip DFP and ARM::CMSIS) and writes `output/SeeedStudio.WioTerminal_BSP.<version>.pack`. Install it with
`cpackget add output/SeeedStudio.WioTerminal_BSP.<version>.pack`.

To use the pack source without installing it, reference it with a path in the csolution:

```yaml
  packs:
    - pack: SeeedStudio::WioTerminal_BSP
      path: <path to this folder>
```

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

The application must start at `0x00004000`, behind the UF2 boot loader; see
[Examples/Blinky/README.md](Examples/Blinky/README.md) for the linker files and `pyocd_user.py`.
For SDK component selection, clock/interrupt ownership and explicit peripheral initialization, see
[Documents/SDK.md](Documents/SDK.md). External power controls and radio enable are not turned on by the
SDK, and the external-flash driver has no program or erase support.
Storage mounts read-only unless explicitly requested otherwise. Networking probes an already-running
radio without reset; joining a network is explicit and may persist credentials in the radio's flash.

## Debugging

The Wio Terminal has no on-board debug probe: connect a CMSIS-DAP probe (for example ULINKplus) to the SWD test
pads SWCLK, SWDIO, RESET, 3V3 and GND on the front side of the PCB. The Blinky example is configured for the Arm
CMSIS Debugger with pyOCD (`CMSIS-DAP@pyOCD`, SWD, 2 MHz). Two problems of `Microchip::SAMD51_DFP@3.9.276`
(flash algorithm sector table, missing `ResetHardware` debug sequence) and their workarounds are described in
[Documents/OVERVIEW.md](Documents/OVERVIEW.md); the proposed DFP changes are in
[Documents/SAMD51_DFP_Proposal.md](Documents/SAMD51_DFP_Proposal.md).

## License

Unless otherwise noted, Apache-2.0, see [LICENSE](LICENSE). The bundled FatFs retains its
[upstream license](Middleware/FatFs/LICENSE.txt); SD protocol implementation attribution and the
permissive ChaN/SdFat notices are in [wio_sd.c](Board/SDK/wio_sd.c) and the
[storage provenance](Middleware/FatFs/PROVENANCE.md).
`Flash/SAMD51_512_8KB.FLM` is derived from `SAMD51_512.FLM` of
`Microchip::SAMD51_DFP` (Apache-2.0, Copyright Microchip Technology Inc.); only its sector table was changed.
