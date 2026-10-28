# Seeed Studio Wio Terminal Board Support Pack

Source of the CMSIS-Pack `SeeedStudio::WioTerminal_BSP`: board support for the Seeed Studio Wio Terminal on top of
the Microchip `SAMD51_DFP`. See [Documents/OVERVIEW.md](Documents/OVERVIEW.md) for the content.

## Repository structure

| Directory / File                       | Content
|:---------------------------------------|:---------------------------------------------------------------
| `SeeedStudio.WioTerminal_BSP.pdsc`     | Pack description (board, flash algorithm, VIO and GPIO components, example)
| `Board/`, `Board/Include/`             | CMSIS-Driver VIO and the pin map `wio_terminal.h`
| `Flash/`                               | `SAMD51_512_8KB.FLM`, the DFP flash algorithm with 8 KB sectors
| `Examples/Blinky/`                     | Blinky example (VIO) in csolution format
| `Documents/`                           | Pack overview and the changes proposed for the Microchip DFP
| `.knowledge/`                          | Knowledge units for the CMSIS Developer Assistant knowledge base
| `.github/workflows/`                   | CI: generate the pack with `gen_pack.sh` (`pack.yml`, release on a `v*` tag), generate and install it and build Blinky with GCC (`build.yml`)
| `gen_pack.sh`                          | Pack generation script ([gen-pack](https://github.com/Open-CMSIS-Pack/gen-pack) library)
| `vcpkg-configuration.json`             | Tool environment (CMSIS-Toolbox, GCC, CMake, Ninja) for the Arm Environment Manager in VS Code

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

## Debugging

The Wio Terminal has no on-board debug probe: connect a CMSIS-DAP probe (for example ULINKplus) to the SWD test
pads SWCLK, SWDIO, RESET, 3V3 and GND on the front side of the PCB. The Blinky example is configured for the Arm
CMSIS Debugger with pyOCD (`CMSIS-DAP@pyOCD`, SWD, 2 MHz). Two problems of `Microchip::SAMD51_DFP@3.9.276`
(flash algorithm sector table, missing `ResetHardware` debug sequence) and their workarounds are described in
[Documents/OVERVIEW.md](Documents/OVERVIEW.md); the proposed DFP changes are in
[Documents/SAMD51_DFP_Proposal.md](Documents/SAMD51_DFP_Proposal.md).

## License

Apache-2.0, see [LICENSE](LICENSE). `Flash/SAMD51_512_8KB.FLM` is derived from `SAMD51_512.FLM` of
`Microchip::SAMD51_DFP` (Apache-2.0, Copyright Microchip Technology Inc.); only its sector table was changed.
