# Blinky project

The **Blinky** project is a simple example that can be used to verify the
basic tool setup for the Seeed Studio Wio Terminal.

It is compliant to the Cortex Microcontroller Software Interface Standard (CMSIS)
and runs bare-metal: the SysTick timer provides a 1 ms time base.

## Operation

- vioLED0 (blue user LED) blinks with 1 Hz.
- While one of the buttons A, B, C or the 5-way switch is pressed, it blinks with 5 Hz.

### CMSIS-Driver Virtual I/O mapping

| CMSIS-Driver VIO      | Board component
|:----------------------|:--------------------------------------
| vioBUTTON0            | Button A (PC26)
| vioBUTTON1            | Button B (PC27)
| vioBUTTON2            | Button C (PC28)
| vioJOYup              | 5-way switch up (PD20)
| vioJOYdown            | 5-way switch down (PD08)
| vioJOYleft            | 5-way switch left (PD12)
| vioJOYright           | 5-way switch right (PD09)
| vioJOYselect          | 5-way switch press (PD10)
| vioLED0               | Blue user LED (PA15)

## Memory layout

The Wio Terminal ships with a 16 KB UF2 boot loader at `0x00000000`, protected by the BOOTPROT fuse. The
application therefore starts at `0x00004000`; the boot loader jumps there after a reset and sets `SCB->VTOR`.
The device startup files of the DFP are configuration files in `RTE/<compiler>/Device/ATSAMD51P19A/`, one
directory per compiler (`rte: base-dir: RTE/$Compiler$` in `Blinky.cproject.yml`) because the Arm Compiler and
GCC variants have the same file names. Their linker files differ from the DFP versions only in the ROM region:

| Compiler | Linker file                     | ROM region
|:---------|:--------------------------------|:-------------------------------
| AC6      | `samd51p19a_flash.sct`          | `__ROM_BASE 0x00004000`, `__ROM_SIZE 0x0007C000`
| GCC      | `samd51p19a_flash.ld`           | `ROM_ORIGIN 0x00004000`, `ROM_LENGTH 0x7C000`

## Build

The project is in *csolution format* for the [CMSIS-Toolbox](https://open-cmsis-pack.github.io/cmsis-toolbox/)
and the VS Code [Arm CMSIS Solution](https://marketplace.visualstudio.com/items?itemName=Arm.cmsis-csolution) extension.

```sh
cbuild Blinky.csolution.yml --packs --update-rte --context Blinky.Debug+Wio-Terminal --toolchain GCC
```

## Run and debug

The Wio Terminal has no on-board debug probe. Connect a CMSIS-DAP probe (for example ULINKplus) to the SWD
test pads SWCLK, SWDIO, RESET, 3V3 (VTref) and GND on the front side of the PCB (casing removed). The solution
is configured for the Arm CMSIS Debugger with pyOCD (`CMSIS-DAP@pyOCD`, SWD, 2 MHz).

`pyocd_user.py` makes pyOCD program the internal Flash with the board's flash algorithm
(`Flash/SAMD51_512_8KB.FLM` of the pack). The algorithm of `Microchip::SAMD51_DFP@3.9.276` declares the whole
Flash as one 512 KB sector, so pyOCD always erases the block at `0x00000000`, which BOOTPROT protects, and the
download fails with `flash erase sector failure (address 0x00000000; result code 0x1)`. csolution lists the board
algorithm after the device algorithm and pyOCD uses the first one; the script selects the board algorithm. pyOCD
reads it from the folder of the solution, which is the working directory of the CMSIS Load, Run and Erase tasks.

Without a probe: start the boot loader by sliding the power switch to the reset position twice in quick
succession (see the Getting Started page), convert the `.hex` file to UF2 (for example with
`uf2conv.py -f 0x55114460 -b 0x4000`) and copy it to the USB drive of the boot loader. This was not tried
during the validation below.

### Hardware reset

A hardware reset (nRESET) of the ATSAMD51 also resets its SW-DP and clears DHCSR and DEMCR. pyOCD's default
hardware reset does not reconnect, so a `monitor reset halt hardware` in a debug session ends with
`SWD/JTAG communication failure (Unexpected ACK '0')`. Use the system reset (the default of the launch
configuration) in a debug session. The debug sequence that handles the hardware reset belongs to the DFP; see
`Documents/SAMD51_DFP_Proposal.md` of the pack.

### Verified on hardware

Wio Terminal with ULINKplus (CMSIS-DAP, SWD at 2 MHz) on the SWD pads, Arm Compiler 6.24 and GCC 14.3.1,
CMSIS-Toolbox 2.15 (csolution 2.15.1), pyOCD 0.46.0 via the Arm CMSIS Debugger 1.10.0: download behind the
boot loader, run to `main`, SysTick timing (`msTicks`), PA15 output toggling every 500 ms and the idle level
of the button and switch inputs checked with the debugger. LED and buttons were not observed visually.
