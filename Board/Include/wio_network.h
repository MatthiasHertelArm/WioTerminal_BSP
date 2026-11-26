/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WIO_NETWORK_H_
#define WIO_NETWORK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bare-metal IPv4 client for Seeed RTL8720DN eRPC firmware 2.1.3 only.
 * Requires wio_sdk_init(), enabled interrupts and exclusive ownership of
 * Driver_USART0 (SERCOM0, also used by Driver_SPI0). No Arduino, heap or RTOS.
 * All calls are serialized foreground operations; callbacks must not reenter.
 * Every timeout is a TOTAL call deadline, 1..120000 ms, except poll(0).
 * The timeout bounds the host, not an operation already executing on the radio.
 * A framing/CRC/UART/RPC timeout fault closes only the host UART: CHIP_PU is
 * NEVER driven here, because radio flash writes may still be in progress.
 * Do not reset/power down the radio to recover until its operation is quiescent.
 * Reinitialization does not resynchronize an interrupted stream or reset the
 * firmware. The application must establish a clean, quiescent peer first.
 */
#define WIO_NETWORK_FIRMWARE_VERSION "2.1.3"
#define WIO_NETWORK_MAX_TRANSFER     1024U
#define WIO_NETWORK_MAX_SOCKETS      4U
#define WIO_NETWORK_MAX_TIMEOUT_MS   120000U
#define WIO_NETWORK_INVALID_SOCKET  0U

typedef enum {
  WIO_NETWORK_OK = 0,
  WIO_NETWORK_ARGUMENT = -1,
  WIO_NETWORK_STATE = -2,
  WIO_NETWORK_BUSY = -3,
  WIO_NETWORK_TIMEOUT = -4,
  WIO_NETWORK_IO = -5,
  WIO_NETWORK_PROTOCOL = -6,
  WIO_NETWORK_FIRMWARE = -7,
  WIO_NETWORK_REMOTE = -8,
  WIO_NETWORK_WOULD_BLOCK = -9,
  WIO_NETWORK_NO_SOCKET = -10,
  WIO_NETWORK_UNSUPPORTED = -11
} wio_network_result_t;

typedef enum {
  WIO_NETWORK_OFF = 0,
  WIO_NETWORK_READY,
  WIO_NETWORK_FAULTED
} wio_network_state_t;

typedef struct { uint8_t octet[4]; } wio_network_ipv4_t;
typedef struct {
  wio_network_ipv4_t address;
  uint16_t port;                    /* Host byte order. */
} wio_network_endpoint_t;

typedef struct {
  char firmware[16];
  uint32_t uart_baud;
  uint16_t max_transfer;
  uint8_t max_sockets;
  bool echo_ok;
} wio_network_info_t;

typedef enum {
  WIO_NETWORK_SECURITY_OPEN = 0,
  WIO_NETWORK_SECURITY_WPA2_AES_PSK = 1
} wio_network_security_t;

typedef struct {
  bool associated;
  bool has_ipv4;
  wio_network_ipv4_t address, netmask, gateway;
} wio_network_status_t;

/* Wi-Fi event IDs are Seeed system_event_id_t values 0..25. Only explicitly
 * decoded, pointer-free fields are exposed; all other union bytes are ignored.
 * Callback data is valid only for the duration of the callback. Delivery is
 * synchronous inside a network call/poll, never in a UART interrupt.
 */
typedef struct {
  uint32_t id;
  uint8_t disconnect_reason;        /* Valid only for id 5. */
  wio_network_ipv4_t address;        /* Valid only for id 7. */
} wio_network_event_t;
typedef void (*wio_network_callback_t)(const wio_network_event_t *event,
                                       void *context);

typedef struct {
  wio_network_state_t state;
  wio_network_result_t last_result;
  int32_t remote_result;
  int32_t remote_errno;             /* Snapshot, may be shared by peer tasks. */
  uint32_t rpc_count, event_count;
} wio_network_diagnostics_t;

/* Probe only: version + echo. Does not enable/reset the radio, scan, join,
 * disconnect, initialize TCP/IP or change stored credentials. An already
 * running radio is required. Its own boot-time reconnect policy is outside
 * this client; an existing firmware/profile may already be associated.
 * info is cleared first and includes a received incompatible version on failure.
 */
wio_network_result_t wio_network_init(wio_network_info_t *info,
                                      wio_network_callback_t callback,
                                      void *context, uint32_t timeout_ms);
/* Host detach only. Close sockets and disconnect explicitly first if desired.
 * No remote resources are automatically destroyed, including after a fault.
 */
wio_network_result_t wio_network_deinit(void);
wio_network_state_t wio_network_get_state(void);
wio_network_diagnostics_t wio_network_get_diagnostics(void);
/* poll(0) processes at most eight already-buffered callbacks without waiting.
 * A partial frame is retained. It faults if still incomplete at a parser check
 * at least 500 ms after its first byte was observed; arrival times are not logged.
 * Call regularly while idle; the finite UART ring cannot absorb unbounded data.
 */
wio_network_result_t wio_network_poll(uint32_t timeout_ms);

/* Explicit station setup + DHCP. SSID 1..32 bytes (no embedded NUL), WPA2
 * password 8..63 printable ASCII bytes; OPEN requires NULL/empty password.
 * Changes the radio Wi-Fi mode and disables automatic reconnect for this run.
 * IMPORTANT: the supported firmware may SAVE SSID/password in internal flash
 * after a successful join; this API cannot offer an ephemeral-credentials mode.
 * No socket may be open when joining or disconnecting. The host keeps no
 * credential profile, and scrubs the serialized TX buffer; the application
 * still owns its original credential strings.
 */
wio_network_result_t wio_network_join(const char *ssid, const char *password,
                                      wio_network_security_t security,
                                      uint32_t timeout_ms);
wio_network_result_t wio_network_disconnect(uint32_t timeout_ms);
wio_network_result_t wio_network_get_status(wio_network_status_t *status,
                                            uint32_t timeout_ms);
wio_network_result_t wio_network_resolve(const char *hostname,
                                         wio_network_ipv4_t *address,
                                         uint32_t timeout_ms);

typedef uint32_t wio_network_socket_t; /* Opaque generation-checked host handle. */
typedef enum {
  WIO_NETWORK_TCP = 1,
  WIO_NETWORK_UDP = 2
} wio_network_socket_type_t;

/* IPv4 TCP client and UDP only; no listen/accept, IPv6, TLS, raw sockets or BLE.
 * Open/bind/connect/close have explicit remote side effects. TCP connect can
 * block on the peer; host timeout faults the session and does not cancel it.
 * send/receive use the peer's nonblocking flag; WOULD_BLOCK is not a fault.
 * A positive partial send is reported exactly once and is never retried here.
 * TCP receive returning OK with count zero means orderly EOF. UDP zero-length
 * datagrams can be received, but zero-length sends are rejected: the peer's
 * generated decoder may reject a zero-byte allocation without replying.
 * Datagrams larger than the supplied receive capacity
 * can be truncated/discarded by the peer and cannot be detected by this ABI.
 * Buffers need remain valid only until these synchronous functions return.
 */
wio_network_result_t wio_network_socket_open(wio_network_socket_type_t type,
                                            wio_network_socket_t *socket,
                                            uint32_t timeout_ms);
wio_network_result_t wio_network_socket_connect(wio_network_socket_t socket,
                                               const wio_network_endpoint_t *peer,
                                               uint32_t timeout_ms);
wio_network_result_t wio_network_socket_bind(wio_network_socket_t socket,
                                            const wio_network_endpoint_t *local,
                                            uint32_t timeout_ms);
wio_network_result_t wio_network_socket_send(wio_network_socket_t socket,
                                            const void *data, size_t length,
                                            size_t *sent, uint32_t timeout_ms);
wio_network_result_t wio_network_socket_receive(wio_network_socket_t socket,
                                               void *data, size_t capacity,
                                               size_t *received,
                                               uint32_t timeout_ms);
wio_network_result_t wio_network_socket_sendto(wio_network_socket_t socket,
                                              const void *data, size_t length,
                                              const wio_network_endpoint_t *peer,
                                              size_t *sent, uint32_t timeout_ms);
wio_network_result_t wio_network_socket_recvfrom(wio_network_socket_t socket,
                                                void *data, size_t capacity,
                                                wio_network_endpoint_t *peer,
                                                size_t *received,
                                                uint32_t timeout_ms);
wio_network_result_t wio_network_socket_close(wio_network_socket_t socket,
                                             uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif
