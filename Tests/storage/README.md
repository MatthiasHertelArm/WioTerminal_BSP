# Storage host tests

These tests never open a real disk, serial port, probe or board. They compile the
production SD transport and filesystem bridge with host-only GPIO/time shims.
Use a native host compiler, not the Cortex-M toolchain.

`storage_sd` uses the genuine CMSIS SPI driver interface and an asynchronous SPI
card model. The model consumes/produces buffers only when status is polled,
checks command/data CRCs independently, and simulates SD v1/v2, byte/block
addressing, media protection, capacity limits, failed transfers and stuck
hardware/card/timebase conditions. It rejects unexpected commands, including
erase/trim commands. Both default and explicitly enabled writes are checked.

`storage_fs` compiles the actual vendored, patched FatFs, not a filesystem stub.
Its synthetic FAT12 image and sparse FAT16/FAT32 fixtures exist solely in host RAM. Tests
read files/directories, create/write/close/remount/read a file, exercise the
upstream malformed-volume fixes, and verify readonly/failed-mount gates and
protection of open or dirty objects, physical volume bounds, and failed optional
FAT32 FSInfo reads. A final injected write failure intentionally
leaves its failed handle locked until process exit, checking that no automatic
remount can discard it. No production formatter is included or called.

Build from the repository root with an installed ARM::CMSIS header directory:

```sh
cmake -S Tests/storage -B /tmp/wio-storage-host-tests \
  -DCMSIS_DRIVER_INCLUDE_DIR=/absolute/path/to/CMSIS/Driver/Include \
  -DWIO_STORAGE_SANITIZE=ON
cmake --build /tmp/wio-storage-host-tests
ctest --test-dir /tmp/wio-storage-host-tests --output-on-failure
```

Use a fresh, task-specific build directory. The root [test project](../CMakeLists.txt)
also includes these targets. Sanitizers are supported with native Clang/GCC.
CMSIS headers are not duplicated in the test shims. Each CTest has a 30-second
process timeout in addition to the transport's own timeout/spin limits.

Passing these simulations establishes software behavior, not electrical/card
compatibility. The hardware project deliberately mounts read-only; actual file
write validation requires separate explicit permission and a disposable card.
