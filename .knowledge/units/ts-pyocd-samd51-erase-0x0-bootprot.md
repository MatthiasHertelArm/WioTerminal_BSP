---
id: ts-pyocd-samd51-erase-0x0-bootprot
type: troubleshooting
title: "pyOCD: flash erase sector failure (address 0x00000000; result code 0x1) on an ATSAMD51 with a boot loader"
summary: "SAMD51_512.FLM of Microchip::SAMD51_DFP@3.9.276 declares the whole Flash as one sector, so pyOCD erases the block at 0x00000000 for every download; BOOTPROT protects it on boards with a UF2 boot loader. Program with the 8 KB-sector algorithm of SeeedStudio::WioTerminal_BSP through pyocd_user.py."
domain: debug
status: draft
tool: pyocd
severity: error
symptoms:
  - "flash erase sector failure (address 0x00000000; result code 0x1)"
  - "'CMSIS Load' exited with code 1"
symptom_regex: "flash erase sector failure \\(address 0x0+; result code 0x1\\)"
applies_to:
  tool: [pyocd, cmsis-debugger]
  device: [ATSAMD51P19A]
  board: [Wio-Terminal]
  pack: [Microchip::SAMD51_DFP]
source: own observation 2026-10-28 on a Wio Terminal (ULINKplus, pyOCD 0.46.0); FlashDevice table and EraseSector disassembly of SAMD51_512.FLM
license: Apache-2.0
confidence: high
---

## Symptom

`CMSIS Load` (or the `flash` tool) fails right at the start of a download with
`flash erase sector failure (address 0x00000000; result code 0x1)`, also when the image is linked at
`0x00004000` behind the boot loader.

## Cause

The `FlashDevice` sector table of `samd51a/keil/Flash/SAMD51_512.FLM` (DFP 3.9.276) has one entry: sectors of
`0x80000` from `0x0`. Its `EraseSector` erases only the 8 KB NVM block at the given address. pyOCD follows the
table, so the only "sector" it ever erases starts at `0x00000000`. On the Wio Terminal the UF2 boot loader there
is protected by BOOTPROT = 13 (NVMCTRL.STATUS.BOOTPROT), the erase is refused and the algorithm returns 1. On a
board without boot loader the same table means that everything above the first 8 KB is programmed without
being erased. `SAMD51_256.FLM` and `SAMD51_1024.FLM` have the same one-sector table. The algorithms have no
`EraseChip`.

## Fix

- With `SeeedStudio::WioTerminal_BSP`: copy `Examples/Blinky/pyocd_user.py` of the pack next to the
  `*.csolution.yml`. The pack describes `Flash/SAMD51_512_8KB.FLM` (the same algorithm with 8 KB sectors) as a
  board algorithm; csolution lists it in `*.cbuild-run.yml` after the device algorithm and pyOCD uses the first
  algorithm that covers a region. The script makes pyOCD use the board algorithm. pyOCD loads it from its project
  directory, which is the working directory of the CMSIS Load, Run and Erase tasks (the workspace folder).
- Not a fix: a `memory:` entry with its own algorithm in the csolution target-type; it is also listed after the
  device algorithm.
- The CMSIS Developer Assistant `flash` tool runs pyOCD outside the workspace folder and does not load
  `pyocd_user.py`; use `cmsis_action load` or `load_and_debug`.

## Evidence

Verified on 2026-10-28: the download failed with the DFP algorithm and succeeded with the 8 KB algorithm (via the
user script) for Blinky (AC6 and GCC) and a minimal project; the boot loader kept running and started the
application at `0x00004000`. The DFP change is proposed in `Documents/SAMD51_DFP_Proposal.md` of the pack.
