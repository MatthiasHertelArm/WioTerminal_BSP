# FatFs R0.16, official patches 1 and 2

Imported 2026-10-28 from the author's distribution, not a third-party fork.
Upstream source dates identify R0.16 as 2025-07-22; patch 1 is 2025-09-13 and
patch 2 is 2026-07-10. The original licence and copyright headers are retained.
See [LICENSE.txt](LICENSE.txt) and [SD_NOTICES.txt](SD_NOTICES.txt).

## Pinned inputs

| Input | Official URL | SHA-256 of downloaded bytes |
| --- | --- | --- |
| R0.16 | <https://elm-chan.org/fsw/ff/arc/ff16.zip> | `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e` |
| Patch 1 | <https://elm-chan.org/fsw/ff/patch/ff16p1.diff> | `7996ddc3135f3d534a8153f7d75d587030e5c8551b003c3b7ee823ffb7fc64bf` |
| Patch 2 | <https://elm-chan.org/fsw/ff/patch/ff16p2.diff> | `5cd39f1fc299f0f1dec9fa1ce544dc0bc048b2f6eb3b8161476ed20f9cf1e290` |
| SD SPI reference samples | <https://elm-chan.org/fsw/ff/ffsample.zip> | `9f0d39ae4d6548d852ef737a144acd2307f1bedc6e763e1759b9d996d00a6ef2` |

The supplied upstream diffs are retained in [patches/](patches/), and both were
applied in order with exact context checks. Patch 2 includes the July 2026
security fixes listed by the author for CVE-2026-6682 / CVE-2026-6687, including
the FAT-size overflow bound and the exFAT cluster-count/volume-label bounds.
The complete patch is included even though this profile disables exFAT.
The patch list was checked at <https://elm-chan.org/fsw/ff/patches.html>.

Vendored inputs are `source/ff.c`, `source/ff.h`, `source/ffconf.h`,
`source/diskio.h`, and `LICENSE.txt` from the archive. Text line endings were
normalized from CRLF to LF. No upstream `diskio.c` sample is compiled: the board
bridge in [wio_fs.c](../../Board/SDK/wio_fs.c) implements its actual block I/O.
`ffunicode.c` and `ffsystem.c` are not needed for this configuration.

## Local integration changes

Only one functional addition to the patched upstream core/header is made:
`wio_fatfs_open_objects(const FATFS *)` reads the existing `Files[]` lock table
under `FF_FS_LOCK` and counts occupied entries for a volume. It does not change
locks, flush or invalidate anything. Applications serialize it with all other
FatFs operations; this profile is deliberately non-reentrant.
The exact local core/header patch is retained as
[wio-open-objects.diff](patches/wio-open-objects.diff), applied after official
patch 2; profile configuration is separate in [ffconf.h](ffconf.h).

The query lets [wio_fs_unmount](../../Board/SDK/wio_fs.c) return `FR_LOCKED`
before upstream `f_mount(NULL, ...)` would discard open-object locks. Multiple
read handles for one file share a lock entry; any nonzero count blocks unmount.
Root-directory handles have no upstream lock entry, so they are not counted:
applications must close **all** directory handles before unmounting.

Configuration changes from the shipped upstream `ffconf.h` are:

- `FF_CODE_PAGE=437`: fixed OEM 8.3 names.
- `FF_FS_NORTC=1`, `FF_NORTC_YEAR=2026`: fixed 2026-01-01 timestamps, not a clock.
- `FF_FS_NOFSINFO=3`: do not trust FAT32 free-cluster/next-free hints.
- `FF_FS_LOCK=8`: protect eight distinct open file/non-root-directory objects.

The existing defaults retain one volume, 512-byte sectors, FAT12/16/32, normal
per-file buffers, no LFN/exFAT, no reentrancy/RTOS dependencies, and no formatter,
partition creator or trim command. `FF_FS_READONLY` remains zero so an application
can explicitly opt into writes at runtime; it is not permission to write by
default.

## Ownership and data safety

Mount with `wio_fs_mount(false)` for the default read-only policy. Only a
successful `wio_fs_mount(true)` enables the separate block-layer write gate,
after read-only filesystem inspection. A failed fresh mount and a successful
unmount close the gate. A refused `FR_LOCKED` mount/unmount changes neither the
existing mount nor the gate. Open files remain protected even after I/O failure.
The bridge also validates the mounted FAT/root/data extents and their ordering
against the physical sector count with 64-bit arithmetic, before enabling file
access or writes; oversized or wrapped layouts are rejected as `FR_NO_FILESYSTEM`.

Use only the Wio wrapper for mounting/unmounting, not raw `f_mount`/`f_unmount`;
do not call raw SD or `Driver_SPI6` while the filesystem owns the volume.
Serialize all filesystem/SD calls in thread mode with interrupts enabled.
Use standard `f_open`, `f_read`, `f_readdir`, `f_write`, `f_sync`, `f_close`, etc.
Close every file/directory before unmounting or removing a card. Dirty cached
metadata and failed media syncs also prevent unmounting. There is no forced
unmount that silently throws away unwritten data.

After an I/O fault, `disk_initialize` reports state; it does not reset the card
and transparently remount over dirty handles. Preserve the error, stop access,
and arrange explicit application recovery/reboot if a failed file cannot be
closed. Power loss/card removal can still lose data: this is FAT, not a journaled
or atomic transaction system. Raw multi-block writes may partly complete.

The filesystem profile cannot represent a physical sector count of exactly
2^32; it rejects that geometry even though the raw SD layer uses a 64-bit
capacity and can address its final 32-bit LBA. exFAT/GPT/SDUC/MMC are not supported.
There is no card-detect polarity assumption, automatic format, erase or trim.
SDXC cards need an already-existing supported FAT partition; the SDK never
reformats a card. This does not alter the external Winbond Flash read-only policy.

## SD protocol references

[wio_sd.c](../../Board/SDK/wio_sd.c) uses CMSIS `Driver_SPI6`, <=400 kHz during
initialization and <=4 MHz for data. The initialization/CS sequencing, SDSC
addressing and CSD capacity calculation were cross-checked with the author's
`generic/sdmm.c` in <https://elm-chan.org/fsw/ff/ffsample.zip> (2019 source).
CRC enable, CRC7/CRC16, protection bits, timeouts and status handling were also
checked against [SdFat 2.3.1](https://github.com/greiman/SdFat/tree/cda057318bec196183d4cc92b01bc1dd64bbfb02),
specifically `src/SdCard/SdSpiCard/SdSpiCard.cpp` and `src/SdCard/SdCardInfo.h`.
SdFat is a reference, not a linked runtime dependency. Full associated notices
are in [SD_NOTICES.txt](SD_NOTICES.txt).

## Validation

[Host tests](../../Tests/storage/) compile the real SD SDK against the genuine
CMSIS SPI header and an asynchronous card model, and the real patched FatFs
against a RAM-only disk. They exercise CRC vectors/corruption, init variants,
capacity/address arithmetic, write gates, write errors, bounded/frozen-time
waits, FAT12 read/directory operations, writable persistence, malformed volume
regressions and protection of open/dirty objects. They cannot establish physical
signal integrity or compatibility with every card. Hardware results belong in
the hardware-test report, not in a simulated PASS claim.
