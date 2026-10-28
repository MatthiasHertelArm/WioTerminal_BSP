---
id: ts-pyocd-samd51-hardware-reset-ack0
type: troubleshooting
title: "pyOCD: Unexpected ACK '0' and unreadable registers after a hardware reset of an ATSAMD51"
summary: "An nRESET pulse resets the SW-DP of the SAM D5x/E5x and clears DHCSR and DEMCR; the DFP has no ResetHardware sequence that reconnects, so the session loses the target. Use the system reset; the fix belongs in the DFP."
domain: debug
status: draft
tool: pyocd
severity: error
symptoms:
  - "SWD/JTAG communication failure (Unexpected ACK '0'); check USB cable, reduce debugger clock"
  - "Could not read registers; remote failure reply '01'"
  - "Unhandled exception processing RSP command (g)"
symptom_regex: "Unexpected ACK '0'"
applies_to:
  tool: [pyocd, cmsis-debugger]
  device: [ATSAMD51P19A]
  board: [Wio-Terminal]
  pack: [Microchip::SAMD51_DFP]
source: own measurements 2026-10-28 on a Wio Terminal (ULINKplus, SWD 2 MHz, pyOCD 0.46.0); SAM D5x/E5x data sheet DS60001507N sections 12.6.2 to 12.9
license: Apache-2.0
confidence: high
---

## Symptom

After `monitor reset halt hardware` (the CMSIS Developer Assistant `reset` tool with `method: hardware`, or a
`hardware` reset type) the session stays "halted", but every register read fails: `Unhandled exception
processing RSP command (g): SWD/JTAG communication failure (Unexpected ACK '0')`, `Could not read registers`.
The firmware keeps running on the board (RSTC.RCAUSE = 0x10, external reset).

## Cause

On the SAM D5x/E5x an nRESET pulse also resets the SW-DP and clears DHCSR (C_DEBUGEN) and DEMCR (vector catch).
pyOCD's default hardware reset only pulses nRESET and keeps using the old connection. With SWCLK low while
nRESET is released, the DSU additionally holds the CPU in reset extension (DSU.STATUSA.CRSTEXT = 1) until a
debugger writes 1 to CRSTEXT (external address 0x41002101). A ULINKplus keeps SWCLK high, so with pyOCD's default
reset the CPU runs after the pulse.

Measured with a sequence that drives SWCLK low: after the reset STATUSA = 0x02, DHCSR = 0x02010000,
DEMCR = 0x00000000.

## Fix

- In a debug session use the system reset (SYSRESETREQ, `monitor reset halt`, the default of the CMSIS Debugger
  launch configuration); it halts at the reset vector and runs to `main` reliably.
- `pyocd load` with its default post-reset (`hardware`) is not affected: it disconnects before the pulse and the
  next session connects anew.
- Lasting fix: a `ResetHardware` debug sequence in the DFP that drives SWCLK low during the pulse, reconnects
  the SW-DP (`DebugPortSetup`, `DebugPortStart`), restores DHCSR and DEMCR and then writes DSU.STATUSA.CRSTEXT.
  The tested sequence is in `Documents/SAMD51_DFP_Proposal.md` of `SeeedStudio::WioTerminal_BSP`. A BSP cannot
  carry it: the PACK schema has no `<sequences>` element for boards.
- With pyOCD 0.46 the first system bus access after the reconnect in a sequence must be a byte access (the
  sequence reads DSU.STATUSA): otherwise the first `Write32` did not take effect (3 of 3 runs, core kept running,
  `Failed to halt device on reset (state is RUNNING)`), probably because pyOCD keeps its cached MEM-AP CSW.

## Evidence

2026-10-28, Wio Terminal, ULINKplus: the default hardware reset lost the session; the proposed sequence,
injected into `*.cbuild-run.yml`, halted at the reset vector 7 of 7 times and reached `main` 2 of 2 times.
