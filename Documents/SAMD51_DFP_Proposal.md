# Proposed changes to Microchip::SAMD51_DFP

Status: **proposed**, not yet sent to Microchip. Both changes belong to the Device Family Pack: the PACK schema
allows `<sequences>` only in device properties, and pyOCD uses the device's flash algorithm even when a board
describes another one for the same range (see below). The Wio Terminal BSP works around both until the DFP
contains them.

Found with `Microchip::SAMD51_DFP@3.9.276`, CMSIS-Toolbox 2.15 (csolution 2.15.1), pyOCD 0.46.0 (Arm CMSIS
Debugger 1.10.0), a ULINKplus (CMSIS-DAP, SWD 2 MHz) and a Seeed Studio Wio Terminal (ATSAMD51P19A with a
16 KB UF2 boot loader protected by BOOTPROT = 13).

## 1. Flash algorithms: one sector for the whole Flash

`samd51a/keil/Flash/SAMD51_256.FLM`, `SAMD51_512.FLM` and `SAMD51_1024.FLM` describe the Flash in their
`FlashDevice` structure as a single sector of the full size (0x40000, 0x80000, 0x100000). `EraseSector` of
`SAMD51_512.FLM` erases one 8 KB NVM block at the given address (checked in its disassembly). A tool that follows
the sector table therefore

- erases the block at `0x00000000` for any download, which fails on a device whose boot loader is protected by
  BOOTPROT: pyOCD reports `flash erase sector failure (address 0x00000000; result code 0x1)`;
- programs everything above the first 8 KB without erasing it.

The algorithms have no `EraseChip` function, so a tool cannot fall back to a chip erase either.

**Proposed change:** describe the Flash as 8 KB sectors (`{ 0x2000, 0x000000 }` as the first sector table
entry). `Flash/SAMD51_512_8KB.FLM` of the Wio Terminal BSP is `SAMD51_512.FLM` with exactly this change (two
bytes differ); with it pyOCD erases and programs only the blocks of the image and leaves the boot loader alone.
The 256 KB and 1024 KB variants have the same sector table; their `EraseSector` was not checked.

## 2. Debug sequence ResetHardware

A hardware reset (nRESET) of the SAM D5x/E5x

- resets the SW-DP: the next SWD transfer gets no acknowledge (`Unexpected ACK '0'` in pyOCD) until the debugger
  performs a line reset and powers up the debug port again;
- clears DHCSR (C_DEBUGEN) and DEMCR (vector catch): a reset with halt does not stop at the reset vector;
- holds the CPU in reset extension when SWCLK is low while nRESET is released (data sheet DS60001507N,
  section 12.6.2 *CPU Reset Extension*, p. 93; release by writing 1 to DSU.STATUSA.CRSTEXT).

Measured on the Wio Terminal with the sequence below (RSTC.RCAUSE = 0x10, external reset, after each run):

| Point in the sequence                         | DSU.STATUSA | DHCSR        | DEMCR
|:----------------------------------------------|:------------|:-------------|:------------
| Before nRESET                                 | 0x00        | 0x00030003   | 0x01000401
| After nRESET with SWCLK low, SW-DP re-connected | 0x02 (CRSTEXT) | 0x02010000 | 0x00000000
| After restore and release                     | 0x00        | 0x02030003 (halted at the reset vector) |

pyOCD's default ResetHardware only pulses nRESET; the debug session then fails with `Unexpected ACK '0'`
and `Could not read registers`.

**Proposed change:** add to the `<family Dfamily="SAMD51">` element, after `<debugport>`:

```xml
<sequences>
  <!-- An nRESET pulse also resets the SW-DP and clears DHCSR and DEMCR. Releasing nRESET while SWCLK is
       low holds the CPU in reset extension (DSU.STATUSA.CRSTEXT) until the debugger has re-connected
       and restored its settings. -->
  <sequence name="ResetHardware" info="nRESET with CPU reset extension, SW-DP re-connect and CPU release">
    <block>
      __var nReset      = 0x80;          // DAP_SWJ_Pins: nRESET
      __var SWCLK       = 0x01;          // DAP_SWJ_Pins: SWCLK/TCK
      __var DHCSR       = 0xE000EDF0;    // Debug Halting Control and Status Register
      __var DEMCR       = 0xE000EDFC;    // Debug Exception and Monitor Control Register
      __var DSU_STATUSA = 0x41002101;    // DSU.STATUSA, external (debugger) address range
      __var DSU_STATUSB = 0x41002102;    // DSU.STATUSB, external (debugger) address range
      __var protected   = 0;
      __var pinLevel    = SWCLK;         // SWCLK level while nRESET is released
      __var dhcsr       = 0;
      __var demcr       = 0;
      __var crstext     = 0;
      __var canReadPins = 0;

      // DSU.STATUSB.PROT: with the NVMCTRL security bit set, only the DSU range is accessible
      protected = Read8(DSU_STATUSB) &amp; 0x01;
    </block>
    <control if="!protected">
      <block>
        // DHCSR.C_DEBUGEN and DEMCR (vector catch, TRCENA) do not survive the external reset
        dhcsr    = Read32(DHCSR);
        demcr    = Read32(DEMCR);

        // SWCLK low at the release requests the CPU reset extension
        pinLevel = 0;
      </block>
    </control>
    <control if="protected">
      <block>
        // A protected device cannot release the reset extension: keep SWCLK high so that the CPU runs
        Message(1, "Device is protected (DSU.STATUSB.PROT): a chip erase removes the protection");
      </block>
    </control>
    <block>
      // Assert nRESET and set SWCLK to the release level
      canReadPins = (DAP_SWJ_Pins(pinLevel, nReset | SWCLK, 0) != 0xFFFFFFFF);
      DAP_Delay(50000);

      // Release nRESET, SWCLK keeps its level
      DAP_SWJ_Pins(nReset, nReset, 0);
    </block>
    <control if="canReadPins" while="(DAP_SWJ_Pins(nReset, nReset, 0) &amp; nReset) == 0" timeout="1000000"/>
    <block>
      DAP_Delay(10000);

      // The external reset also resets the SW-DP: line reset, DPIDR read and debug power-up
      Sequence("DebugPortSetup");
      Sequence("DebugPortStart");
    </block>
    <control if="!protected">
      <block>
        // DSU.STATUSA.CRSTEXT: the CPU is held in reset extension. Being the first system bus access
        // after the re-connect, this byte read also makes the debugger set up the MEM-AP again.
        crstext = Read8(DSU_STATUSA) &amp; 0x02;

        // Restore halting debug and vector catch
        Write32(DHCSR, 0xA05F0000 | (dhcsr &amp; 0x1));
        Write32(DEMCR, demcr);
      </block>
      <control if="crstext">
        <block>
          // Release the CPU: write 1 to DSU.STATUSA.CRSTEXT
          Write8(DSU_STATUSA, 0x02);
        </block>
      </control>
    </control>
  </sequence>
</sequences>
```

The other sequences keep their defaults: `ResetSystem` (SYSRESETREQ) halted at the reset vector and ran to
`main` in every test, so it needs no change.

### Validation

- **XML:** the block above inserted into a copy of the 3.9.276 PDSC: `packchk -i ARM.CMSIS.pdsc` reports
  0 errors and the same 19 warnings (M510) as the unchanged PDSC; `xmllint` well-formed; no one-line `<block>`
  and one statement per line (the only `;…;` matches are `&amp;` entities).
- **Hardware:** the sequence was given to pyOCD in the `debug-sequences` node of a `*.cbuild-run.yml` (as
  csolution writes it from a DFP). Reset with halt (`monitor reset halt hardware`) stopped at the boot loader's
  reset vector (PC = 0x0000056C) in 7 of 7 runs, reset with run reached `main` in 2 of 2 runs, and
  `pyocd load` with its default post-reset (`hardware`, which uses this sequence) was followed by a debug
  session that reached `main` every time.
- **The byte read before the 32-bit writes is required with pyOCD 0.46:** without it the core ran on after the
  reset in 3 of 3 runs (`Failed to halt device on reset (state is RUNNING)`). The most likely cause is that
  pyOCD keeps its cached MEM-AP CSW across the SW-DP re-connect, so the first 32-bit write uses the reset value
  of CSW; the byte access makes pyOCD write CSW again. Worth reporting to pyOCD as well.

### Not covered

- The protected path (`DSU.STATUSB.PROT = 1`) follows the data sheet (sections 12.6.2, 12.7, 12.9) and was not
  run: the test device is not protected.
- `ResetHardwareAssert` / `ResetHardwareDeassert` (connect under reset) keep their defaults and were not tried.
- pyOCD probes AP#2..AP#4 at every connect and logs `Error probing AP#n ... FAULT ACK`; the device has two APs.
  Describing them with `<accessportV1>` in the DFP may silence this; not tried.
