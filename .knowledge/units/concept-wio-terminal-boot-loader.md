---
id: concept-wio-terminal-boot-loader
type: concept
title: "Wio Terminal: UF2 boot loader, application offset and double-tap window"
summary: "16 KB UF2 boot loader (uf2-samdx1) at 0x0, protected by BOOTPROT = 13; applications start at 0x00004000, where the boot loader sets VTOR and jumps. A second non-power-on reset within 500 ms keeps it in boot loader mode."
domain: board
status: draft
applies_to:
  board: [Wio-Terminal]
  device: [ATSAMD51P19A]
  pack: [SeeedStudio::WioTerminal_BSP, Microchip::SAMD51_DFP]
source: NVMCTRL and user row read on a Wio Terminal 2026-10-28; adafruit/uf2-samdx1 src/main.c; Seeed-Studio/OSHW-WioTerminal (bootloader-wio_terminal-v3.7.0, 16384 bytes)
license: Apache-2.0
---

## Memory layout

| Range | Content |
|:--|:--|
| `0x00000000`..`0x00003FFF` | UF2 boot loader (uf2-samdx1 v3.7.0 for the Wio Terminal, 16 KB) |
| `0x00004000`..`0x0007FFFF` | Application (496 KB) |
| `0x20000000`..`0x2002FFFF` | RAM (192 KB); the last word is used by the boot loader for its double-tap marker |
| `0x04000000` | QSPI Flash W25Q32JV (4 MB) when the QSPI peripheral maps it |

NVMCTRL.STATUS.BOOTPROT = 13 on the tested board: (15 - 13) x 8 KB = 16 KB protected. Erasing or programming that
range fails; changing BOOTPROT needs a user-row write and removes the protection of the boot loader.

## What an application needs

- Link the ROM region at `0x00004000` (AC6 scatter `__ROM_BASE`, GCC `ROM_ORIGIN`); the DFP startup files
  otherwise work unchanged. The boot loader checks the reset vector of the application, loads MSP from
  `0x00004000`, sets `SCB->VTOR = 0x00004000` and branches; the DFP SystemInit (AC6) and Reset_Handler (GCC) set
  VTOR again from the vector table.
- A program for a debugger without the boot loader would start at `0x0`: keep the boot loader to keep UF2
  drag-and-drop working (decision taken for this board on 2026-10-28).

## Start decision of the boot loader (uf2-samdx1 main.c)

- Power-on reset: clears the marker and starts the application.
- Other reset (external, system, watchdog) with the marker set: stays in boot loader mode (USB drive).
- Other reset without the marker: sets the marker, waits 500 ms, clears it and starts the application.
- `main()` stops early when `SCB->VTOR` is not 0; a core reset (VECTRESET) that keeps VTOR at the application's
  table could therefore hang the boot loader (from the source; not tried).

Consequence for debugging: two resets less than 500 ms apart (for example a reset after a download followed
immediately by another reset) leave the board in boot loader mode and `main` is never reached; one more reset
starts the application. A reset with halt stops before the boot loader runs and does not set the marker.
