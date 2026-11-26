# RTL8720DN networking: supported protocol and limits

The [network SDK](../Board/Include/wio_network.h) is an independently implemented,
fixed-storage C client for Seeed's **eRPC firmware 2.1.3**. It is not an AT-command
driver and does not install, update, erase or reflash radio firmware. A matching
version string is a compatibility check, not authentication of the firmware or
proof that every remote feature works.

The [protocol engine](../Board/SDK/wio_network.c) and
[CMSIS UART adapter](../Board/SDK/wio_network_uart.c) require the
[Core SDK](../Board/Include/wio_sdk.h), enabled interrupts and exclusive ownership
of `Driver_USART0`. They do not depend on Arduino, an RTOS, dynamic allocation,
DMA or another SysTick handler. No CMSIS WiFi conformance claim is made: the public
interface is the board SDK, built on the CMSIS USART transport.

## Pinned primary sources

These source revisions were inspected to establish the protocol. Their source
trees and radio binaries are not bundled in the board pack.

| Repository | Revision | Used to establish |
|---|---|---|
| [Seeed_Arduino_rpcUnified](https://github.com/Seeed-Studio/Seeed_Arduino_rpcUnified/tree/f0570f481d6f65c2e86c147f36d2b994d7bea645) | `f0570f481d6f65c2e86c147f36d2b994d7bea645` | UART selection, eRPC framing/CRC/BasicCodec, generated RPC IDs and schemas |
| [Seeed_Arduino_rpcWiFi](https://github.com/Seeed-Studio/Seeed_Arduino_rpcWiFi/tree/4523b1d6e268b53b512df0071c9e88948ed89217) | `4523b1d6e268b53b512df0071c9e88948ed89217` | Supported station/socket workflow; wrappers are not copied |
| [seeed-ambd-firmware](https://github.com/Seeed-Studio/seeed-ambd-firmware/tree/86119d870213a441497ac76b70b432ddd0beae6a) | `86119d870213a441497ac76b70b432ddd0beae6a` | Firmware version, generated server schemas, callbacks, credential persistence and remote implementation behavior |
| [ArduinoCore-ambd](https://github.com/Seeed-Studio/ArduinoCore-ambd/tree/f81bca75e433e35b2d27cadd178596888940fb5a) | `f81bca75e433e35b2d27cadd178596888940fb5a` | Default remote lwIP ABI, IPv4 configuration, socket flags and error numbers |
| [ArduinoCore-samd](https://github.com/Seeed-Studio/ArduinoCore-samd/tree/2e6b174f81b988964ce3811f5b5e39297c2c9c3f) | `2e6b174f81b988964ce3811f5b5e39297c2c9c3f` | Wio Terminal logical pins 84/85 and their SERCOM mapping |

The three eRPC/Wi-Fi repository root licenses are MIT/Seeed 2020, but individual
files have different licenses: the eRPC runtime includes BSD-3-Clause code and
some Arduino Wi-Fi wrappers retain LGPL-2.1-or-later notices. This SDK does not
copy those wrappers or the C++ runtime. Its original C implementation uses the
documented wire facts, constants and standard CRC algorithm; its files carry
Apache-2.0 SPDX identifiers. Consult the applicable upstream file license before
incorporating additional source, rather than relying on a repository root license.

## Physical transport and ownership

| Signal | SAMD51 pin | Function | CMSIS instance |
|---|---|---|---|
| Host eRPC TX | PB24 | SERCOM0 PAD0, peripheral C | `Driver_USART0` |
| Host eRPC RX | PC24 | SERCOM0 PAD2, peripheral C | `Driver_USART0` |
| RTL CHIP_PU | PA18 | Radio power/reset control; **not driven by this SDK** | Application-owned |

Transport is **614400 baud, 8-N-1, no RTS/CTS**. The generic radio programming/log
UART on PC22/PC23 (`Driver_USART1`) is not the eRPC link. SERCOM0 is also used by
`Driver_SPI0`; the board's shared ownership guard rejects powering both at once.
Applications must not separately initialize, receive on, or replace the callback
of `Driver_USART0` while this SDK owns it.

The UART continuously receives into persistent storage. Its ISR immediately
rearms a one-byte CMSIS receive and publishes the byte to an 8192-byte ring
(8191 usable). Foreground code parses messages and delivers callbacks. UART
framing/parity/overrun errors, rearm failure and ring overflow are sticky faults,
not successful short reads. Poll regularly while idle; finite storage cannot
preserve an unlimited stream while application code stops servicing it.

## Exact wire format

All integer fields below are little-endian unless noted. The historical framing
in these revisions is **four bytes**, not the six-byte header used by some newer
eRPC versions.

| Frame field | Size | Meaning |
|---|---|---|
| Payload length | 2 | BasicCodec body size, validated as 8..4096 |
| Payload CRC | 2 | Non-reflected CRC-16, seed `0xEF4A`, polynomial `0x1021`, no final XOR; payload only |
| Message header | 4 | `(1 << 24) \| (service << 16) \| (method << 8) \| type` |
| Sequence | 4 | Host request sequence echoed by its reply |
| Method arguments/result | Variable | Schema-specific, packed without alignment padding |

Invocation type is 0, one-way callback type is 1 and reply type is 2. Strings and
binary values are `u32 length` followed by exactly that many bytes, without a NUL
terminator. Nullable values use a one-byte flag: 0 means present and 1 means null.
The client validates CRC, protocol version, type, service, method, sequence,
schema lengths and complete consumption of the response, including trailing bytes.

There is no synchronization marker and no CRC over the length field. A bad length,
CRC, truncated frame, unexpected invocation or stale/wrong reply therefore faults
the session; the parser does not scan for a guessed next header. Sequence numbers
and socket handle generations are not reset by host detach/reinitialization.

Useful independent check vectors:

```text
CRC("123456789") = 0x89AC
version request, sequence 1:
08 00 7d a6 00 01 01 01 01 00 00 00
version response "2.1.3", sequence 1:
11 00 93 fe 02 01 01 01 01 00 00 00 05 00 00 00 32 2e 31 2e 33
```

The peer uses generated eRPC 1.7.4 shims. An unsupported service/method or decoder
failure may produce **no error reply at all**. Consequently a timeout cannot
identify whether the peer is absent, busy, incompatible or unable to decode a
request. The SDK never probes by inventing unsupported method IDs.

## Supported RPC subset

| Service | Methods used |
|---|---|
| System 1 | 1 version; 2 byte echo |
| Wi-Fi 14 | 1 connect; 3 disconnect; 4 association status; 27 station mode on; 28 Wi-Fi off; 47 set runtime automatic reconnect |
| TCP/IP 15 | 1 stack initialization; 7 IPv4 information; 13 DHCP start; 14 DHCP stop |
| lwIP sockets 16 | 2 bind; 8 close; 9 connect; 12 receive; 14 receive-from; 15 send; 17 send-to; 18 socket; 24 errno snapshot; 25 synchronous DNS |
| Wi-Fi callback 19 | 1 one-way event only |

Supported application features are station OPEN/WPA2-AES-PSK association with
DHCP, association/IPv4 status, IPv4 DNS, TCP clients and UDP. There are four local
socket slots and a 1024-byte limit for each send/receive. No automatic transmit
retry occurs: partial TCP writes return their exact count. A socket handle is an
opaque host token, not a remote file descriptor.

The implementation deliberately excludes scanning, access-point mode, static-IP
configuration, IPv6, listen/accept, TLS, BLE, raw TCP callback APIs, asynchronous
DNS callbacks and CMSIS WiFi/socket compatibility layers. Protocol CRC is not
cryptographic integrity or authentication; applications needing confidential
Internet traffic require an explicitly integrated, validated secure protocol.

### Remote ABI and implementation caveats

- The pinned default RTL core has `LWIP_IPV6=0`. DNS returns **four IPv4 bytes**
  followed by a signed 8-bit lwIP status. A custom build returning a 20-byte
  dual-stack `ip_addr_t` is rejected, even if it reports the same firmware version.
- IPv4 information is 12 bytes: address, netmask, gateway. A socket address is
  16 bytes: length 16, family 2, a two-byte **network-order** port, four IPv4
  bytes, and eight zero bytes. No host C structure layout is sent on the wire.
- Wi-Fi event payloads are 48 bytes under the normal 32-bit Arm ABI, with a
  32-bit ID in 0..25. Only ID, disconnect reason (event 5), and IPv4 address
  (event 7) are exposed; unused union bytes and pointers are never published.
- Remote receive ignores its timeout argument. The SDK passes `MSG_DONTWAIT=8`
  and maps `EAGAIN/EWOULDBLOCK=11` to `WIO_NETWORK_WOULD_BLOCK`. The separate
  errno RPC is only a snapshot and can be affected by other firmware tasks.
- On receive EOF/error, the firmware sends one uninitialized dummy byte. It is
  validated structurally and discarded, never copied into application data.
  TCP EOF is success with count zero. Empty UDP datagrams can be received.
- Empty sends are rejected locally. The generated remote binary decoder calls
  a zero-byte allocator which may return null and fail without an error reply.
  UDP datagrams larger than the requested capacity can be truncated/discarded
  remotely; this ABI does not provide a reliable truncation flag.
- Remote connect, station join, DHCP and DNS can block. A host deadline does not
  cancel work that has already started on the radio.

## Power, credentials and recovery

`wio_network_init()` attaches the UART and issues only version and echo RPCs. It
does not drive CHIP_PU, initialize TCP/IP, scan, join, disconnect or alter stored
credentials. The radio must already be running and its stream quiescent. Calling
initialization while the radio is held off simply fails within its deadline.
The firmware itself has fast-connect/automatic-reconnect support enabled by
default, with implementation partly in prebuilt vendor libraries: **a cold boot
or an existing saved profile cannot be assumed network-silent**.

Joining requires credentials supplied by the application. It explicitly starts
TCP/IP, stops DHCP, switches Wi-Fi to station mode, requests runtime automatic
reconnect off, connects, and starts DHCP. The first explicit DNS/socket-open
operation also initializes TCP/IP if this host session has not done so. These
are not operations performed by the default probe.

**The supported radio firmware may save SSID/password and reconnect data to its
internal flash after a successful join.** This SDK cannot promise ephemeral
credentials and does not read, erase or rewrite that stored profile itself.
Disconnecting does not erase it. Host request storage is scrubbed after the join
and on every public-call exit, but this does not clear the radio's copy or the
application's original credentials.

All mutating calls are serialized, foreground-only operations. Timeouts are total
host-call deadlines (1..120000 ms), with an additional finite stalled-clock guard.
Callbacks run in the calling foreground context, must return promptly and must
not reenter the SDK; attempted reentry returns `WIO_NETWORK_BUSY`. A partial idle
frame is faulted when a parser check finds it still incomplete 500 ms after its
first observed byte; actual interrupt arrival timestamps are not retained.

A fault aborts host UART I/O and invalidates local socket handles, but **never
resets or powers down the radio**. A timed-out join may still be committing flash.
Remote operations may finish later and remote sockets may remain allocated.
An application must establish that the peer is quiescent and the stream is clean
before explicitly reinitializing; blind automatic reinitialization is not a safe
stream-resynchronization or resource-recovery strategy. There is no automatic
rollback after partially completed multi-RPC station setup.

Host detach also leaves association, saved profiles and remote sockets unchanged.
Close owned sockets and disconnect explicitly first when that is the desired
outcome. Joining/disconnecting while local sockets remain open is rejected to
avoid silently abandoning those handles.

## Verification boundary

The [protocol tests](../Tests/network/test_network.c) exercise golden framing,
fragmented input, stale/mismatched responses, corrupt CRC/length/type/schema,
random valid-CRC payloads, callbacks/reentrancy, stalled clocks and wraparound,
total deadlines, station serialization, DNS, socket handles, partial writes,
nonblocking reads, EOF, UDP address layouts and unsupported empty sends.
The [UART tests](../Tests/network/test_network_uart.c) compile the actual adapter
against genuine CMSIS USART headers and test atomic initial arm, repeated ISR
rearm, ring wrap/overflow, sticky RX errors, transfer lifetime and teardown.
Both are host simulations, not an interoperability or radio qualification test.

The [HardwareTest](../Examples/HardwareTest/README.md) default networking smoke
is limited to version/echo and host detach. It must not silently join a network,
reflash the module or supply stored credentials. A successful smoke result proves
the tested UART/system-RPC exchange only; actual association, DHCP, DNS and TCP/UDP
interoperability still require an explicitly authorized network test with
application-provided credentials and a known test endpoint. See the current
HardwareTest report for measured board evidence; this document does not claim
those live network operations have been validated.
