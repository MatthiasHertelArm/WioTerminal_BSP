---
id: proc-wio-terminal-bring-up
type: procedure
title: "wio-terminal-bring-up"
summary: "Build a csolution for the Seeed Studio Wio Terminal behind its UF2 boot loader, download it with pyOCD through a CMSIS-DAP probe on the SWD pads, and run it to main with the CMSIS Developer Assistant tools."
domain: project
status: draft
applies_to:
  board: [Wio-Terminal]
  device: [ATSAMD51P19A]
  tool: [pyocd, cmsis-debugger]
triggers:
  - first download to a Wio Terminal
  - validating a pack or example on the Wio Terminal
preconditions:
  - a CMSIS-DAP probe (for example ULINKplus) wired to the SWD pads SWCLK, SWDIO, RESET, 3V3, GND
  - a csolution with board SeeedStudio::Wio-Terminal, Microchip::SAMD51_DFP and ARM::CMSIS
stop_conditions:
  - "flash erase sector failure (address 0x00000000; result code 0x1): pyocd_user.py is missing or not in the workspace folder (ts-pyocd-samd51-erase-0x0-bootprot)"
  - "Unexpected ACK '0' after a reset: a hardware reset was used in the session (ts-pyocd-samd51-hardware-reset-ack0)"
  - "main never reached and the boot loader's USB drive appears: two resets within 500 ms (concept-wio-terminal-boot-loader)"
references: [concept-wio-terminal-boot-loader, concept-wio-terminal-hardware, ts-pyocd-samd51-erase-0x0-bootprot, ts-pyocd-samd51-hardware-reset-ack0]
source: bring-up of SeeedStudio::WioTerminal_BSP 0.1.0 on 2026-10-28 (ULINKplus, pyOCD 0.46.0, AC6 6.24, GCC 14.3.1)
license: Apache-2.0
---

## Steps

1. Read the boot loader protection before the first download: `read_memory` of NVMCTRL.STATUS (0x41004012,
   2 bytes) in an attached session; BOOTPROT (bits 11..8) = 13 means 16 KB protected. Keep the boot loader.
2. Link the application at `0x00004000`: change the ROM region of the `Device:Startup` linker configuration file
   (AC6 `samd51p19a_flash.sct` `__ROM_BASE 0x00004000`, `__ROM_SIZE 0x0007C000`; GCC `samd51p19a_flash.ld`
   `ROM_ORIGIN 0x00004000`, `ROM_LENGTH 0x7C000`). With both compilers in one project use
   `rte: base-dir: RTE/$Compiler$`, because the DFP's AC6 and GCC startup files have the same names.
3. Copy `Examples/Blinky/pyocd_user.py` of the pack next to the `*.csolution.yml`. It makes pyOCD use the board's
   8 KB-sector flash algorithm instead of the DFP's one-sector algorithm.
4. `cmsis_action build`, then `get_section_layout`: the first LOAD segment must start at `0x00004000`.
5. `cmsis_action load_and_debug`: the launch configuration resets with SYSRESETREQ and stops at `main`. Do not use
   the `flash` tool (it does not load `pyocd_user.py`).
6. Verify: `read_core_registers` (PC in the image), a breakpoint in the main loop, `read_memory` of the PORT
   registers (PORT group A at 0x41008000, B +0x80, C +0x100, D +0x180; DIR +0x00, OUT +0x10, IN +0x20).
7. Reset in the session with `reset` and `method: system`; `method: hardware` loses the connection with
   `Microchip::SAMD51_DFP@3.9.276`.
8. Leave the target running (`continue_execution`) when done.
