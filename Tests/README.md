# Native SDK regression tests

These tests run on a host computer with mocked UART/SPI/GPIO/timing and an in-memory FAT disk. They do
not connect to the Wio Terminal, access a serial port, open a real disk or send network traffic. They
complement, rather than replace, the [hardware checks](../Examples/HardwareTest/README.md).

Use an installed native C compiler and CMake, not the Arm cross-compiler:

```sh
cmake -S Tests -B build/host-tests -DCMAKE_BUILD_TYPE=Debug -DWIO_TEST_SANITIZERS=ON
cmake --build build/host-tests
ctest --test-dir build/host-tests --output-on-failure
```

The genuine CMSIS Driver headers are taken from an installed `ARM::CMSIS` pack under `CMSIS_PACK_ROOT`
or the usual local pack cache. To select another installation, pass
`-DCMSIS_DRIVER_INCLUDE_DIR=/absolute/path/to/CMSIS/Driver/Include`. Nothing is downloaded automatically.
Address/undefined-behavior sanitizers are optional; assertions remain enabled in Release too.

The suites compile production implementations, including the actual FatFs module:

- [SERCOM ownership](sercom/test_sercom0.c): first-acquire interrupt cleanup, exclusive ownership,
  mismatched release, dispatch through all four vectors, and preservation of interrupt masking.
- [Network protocol](network/test_network.c): primary-source wire vectors, version/echo, callbacks,
  station/DNS/socket request and response schemas, stale handles, deadlines, malformed responses and
  bounded parser rejection. Replies are simulated; no RF/network success is implied.
- [Network UART](network/test_network_uart.c): continuously rearmed reception, ring wrap/overflow,
  sticky UART errors, transmit completion and cleanup against the real CMSIS USART API.
- [SD transport](storage/test_sd.c): SD initialization/addressing/CRC, ranges, write opt-in, timeout and
  error behavior against a scripted card through the real CMSIS SPI API.
- [Filesystem](storage/test_fs.c): real FAT mounting, file/directory reads, read-only enforcement,
  opt-in write/read round trips, mount/unmount ownership and simulated media failures in RAM.

CI builds/runs these with sanitizers before building the Cortex-M examples. Its fixture writes are to
RAM only; HardwareTest never writes an SD card or QSPI flash.
