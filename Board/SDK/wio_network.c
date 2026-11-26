/* SPDX-License-Identifier: Apache-2.0 */
/* Independent C implementation of the pinned Seeed eRPC wire protocol.
 * Protocol/ABI provenance and limitations: Documents/NETWORK_PROTOCOL.md.
 * No upstream Arduino/C++/RTOS implementation is incorporated here.
 */
#include <string.h>
#include "wio_network.h"
#include "wio_network_port.h"

#define FRAME_CAPACITY 4096U
#define FRAME_TIMEOUT_MS 500U
#define STALLED_CLOCK_BUDGET 1000000U
#define WIFI_EVENT_BYTES 48U
#define ERPC_VERSION 1U
#define ERPC_ONEWAY 1U
#define ERPC_REPLY 2U
#define SERVICE_SYSTEM 1U
#define SERVICE_WIFI 14U
#define SERVICE_TCPIP 15U
#define SERVICE_SOCKET 16U
#define SERVICE_WIFI_EVENT 19U
#define LWIP_MSG_DONTWAIT 8U

typedef struct {
  uint32_t start, limit, last_tick, stagnant;
} DEADLINE;
typedef struct {
  const uint8_t *bytes;
  size_t length, position;
  bool ok;
} READER;
typedef struct {
  wio_network_socket_t handle;
  int32_t descriptor;
  wio_network_socket_type_t type;
  bool connected;
} SOCKET;

static struct {
  wio_network_diagnostics_t diagnostics;
  wio_network_callback_t callback;
  void *callback_context;
  bool busy, tcpip_initialized;
  uint8_t tx[FRAME_CAPACITY + 4U];
  size_t tx_used;
  bool tx_valid;
  uint8_t rx_header[4], rx[FRAME_CAPACITY];
  size_t header_used, rx_used, rx_length;
  uint32_t frame_start;
  SOCKET sockets[WIO_NETWORK_MAX_SOCKETS];
} network;
/* Deliberately not reset on detach/fault/reinit: delayed replies/stale handles
 * must never become valid simply because the host attached again.
 */
static uint32_t last_sequence, last_handle;

static uint16_t read_le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}
static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void write_le32(uint8_t *p, uint32_t value) {
  for (unsigned i = 0U; i < 4U; ++i) { p[i] = (uint8_t)(value >> (8U * i)); }
}
static uint16_t payload_crc(const uint8_t *data, size_t length) {
  uint16_t value = 0xEF4AU;
  for (size_t i = 0U; i < length; ++i) {
    value ^= (uint16_t)((uint16_t)data[i] << 8);
    for (unsigned bit = 0U; bit < 8U; ++bit) {
      value = (uint16_t)((value & 0x8000U) != 0U ?
                        ((uint32_t)value << 1) ^ 0x1021U : (uint32_t)value << 1);
    }
  }
  return value;
}
static void scrub(void *data, size_t length) {
  volatile uint8_t *p = (volatile uint8_t *)data;
  while (length != 0U) { *p++ = 0U; --length; }
}
static bool timeout_valid(uint32_t timeout) {
  return timeout != 0U && timeout <= WIO_NETWORK_MAX_TIMEOUT_MS;
}
static void deadline_start(DEADLINE *d, uint32_t timeout) {
  d->start = wio_network_port_millis();
  d->last_tick = d->start;
  d->limit = timeout;
  d->stagnant = 0U;
}
static bool deadline_expired(DEADLINE *d) {
  uint32_t now = wio_network_port_millis();
  if ((uint32_t)(now - d->start) >= d->limit) { return true; }
  if (now != d->last_tick) {
    d->last_tick = now;
    d->stagnant = 0U;
  } else if (++d->stagnant >= STALLED_CLOCK_BUDGET) {
    return true;
  }
  return false;
}
static void reset_parser(void) {
  network.header_used = 0U;
  network.rx_used = 0U;
  network.rx_length = 0U;
}
static wio_network_result_t fault(wio_network_result_t result) {
  /* Closing the UART releases IRQ pointers before TX storage is scrubbed.
   * Never reset/power down the peer: it may still be committing credentials.
   */
  wio_network_port_close();
  network.diagnostics.state = WIO_NETWORK_FAULTED;
  memset(network.sockets, 0, sizeof(network.sockets));
  network.tcpip_initialized = false;
  reset_parser();
  return result;
}
static wio_network_result_t begin(DEADLINE *d, uint32_t timeout) {
  if (!timeout_valid(timeout)) { return WIO_NETWORK_ARGUMENT; }
  if (!wio_network_port_thread()) { return WIO_NETWORK_STATE; }
  if (network.busy) { return WIO_NETWORK_BUSY; }
  if (network.diagnostics.state != WIO_NETWORK_READY) { return WIO_NETWORK_STATE; }
  network.busy = true;
  network.diagnostics.remote_result = 0;
  network.diagnostics.remote_errno = 0;
  deadline_start(d, timeout);
  return WIO_NETWORK_OK;
}
static wio_network_result_t finish(wio_network_result_t result) {
  scrub(network.tx, sizeof(network.tx));
  network.diagnostics.last_result = result;
  network.busy = false;
  return result;
}
static size_t bounded_length(const char *text, size_t maximum) {
  if (text == NULL) { return maximum + 1U; }
  size_t count = 0U;
  while (count <= maximum && text[count] != '\0') { ++count; }
  return count;
}
static void request_start(uint8_t service, uint8_t method) {
  network.tx_used = 12U;
  network.tx_valid = last_sequence != UINT32_MAX;
  if (network.tx_valid) { ++last_sequence; }
  write_le32(network.tx + 4U, (ERPC_VERSION << 24) |
                              (uint32_t)service << 16 | (uint32_t)method << 8);
  write_le32(network.tx + 8U, last_sequence);
}
static void put_bytes(const void *data, size_t length) {
  if (length > sizeof(network.tx) - network.tx_used) {
    network.tx_valid = false;
    return;
  }
  if (length != 0U) { memcpy(network.tx + network.tx_used, data, length); }
  network.tx_used += length;
}
static void put_u8(uint8_t value) { put_bytes(&value, 1U); }
static void put_u32(uint32_t value) {
  uint8_t bytes[4];
  write_le32(bytes, value);
  put_bytes(bytes, sizeof(bytes));
}
static void put_blob(const void *data, size_t length) {
  put_u32((uint32_t)length);
  put_bytes(data, length);
}
static uint32_t get_u32(READER *r) {
  if (!r->ok || r->length - r->position < 4U) { r->ok = false; return 0U; }
  uint32_t result = read_le32(r->bytes + r->position);
  r->position += 4U;
  return result;
}
static uint8_t get_u8(READER *r) {
  if (!r->ok || r->position == r->length) { r->ok = false; return 0U; }
  return r->bytes[r->position++];
}
static const uint8_t *get_blob(READER *r, size_t *length) {
  uint32_t count = get_u32(r);
  if (!r->ok || count > r->length - r->position) {
    r->ok = false;
    *length = 0U;
    return NULL;
  }
  const uint8_t *result = r->bytes + r->position;
  r->position += count;
  *length = count;
  return result;
}
static bool reader_done(const READER *r) { return r->ok && r->position == r->length; }

/* One nonblocking parser step. No byte scanning/resynchronization is safe with
 * this historical 4-byte frame format: length has neither CRC nor a sync marker.
 */
static int receive_frame(void) {
  if (wio_network_port_error()) { return (int)fault(WIO_NETWORK_IO); }
  if (network.header_used < sizeof(network.rx_header)) {
    int32_t count = wio_network_port_read(network.rx_header + network.header_used,
                                         sizeof(network.rx_header) - network.header_used);
    if (count < 0) { return (int)fault(WIO_NETWORK_IO); }
    if (count > 0 && network.header_used == 0U) {
      network.frame_start = wio_network_port_millis();
    }
    network.header_used += (size_t)count;
    if (network.header_used == sizeof(network.rx_header)) {
      network.rx_length = read_le16(network.rx_header);
      if (network.rx_length < 8U || network.rx_length > FRAME_CAPACITY) {
        return (int)fault(WIO_NETWORK_PROTOCOL);
      }
    }
  }
  if (network.header_used == sizeof(network.rx_header) &&
      network.rx_used < network.rx_length) {
    int32_t count = wio_network_port_read(network.rx + network.rx_used,
                                         network.rx_length - network.rx_used);
    if (count < 0) { return (int)fault(WIO_NETWORK_IO); }
    network.rx_used += (size_t)count;
  }
  if (wio_network_port_error()) { return (int)fault(WIO_NETWORK_IO); }
  if (network.header_used == sizeof(network.rx_header) &&
      network.rx_used == network.rx_length) {
    if (payload_crc(network.rx, network.rx_length) != read_le16(network.rx_header + 2U)) {
      return (int)fault(WIO_NETWORK_PROTOCOL);
    }
    return 1;
  }
  if (network.header_used != 0U &&
      (uint32_t)(wio_network_port_millis() - network.frame_start) >= FRAME_TIMEOUT_MS) {
    return (int)fault(WIO_NETWORK_TIMEOUT);
  }
  return 0;
}

static wio_network_result_t event_frame(void) {
  if (read_le32(network.rx) != ((ERPC_VERSION << 24) |
                               (SERVICE_WIFI_EVENT << 16) | (1U << 8) | ERPC_ONEWAY)) {
    return fault(WIO_NETWORK_PROTOCOL);
  }
  READER r = {network.rx, network.rx_length, 8U, true};
  size_t length;
  const uint8_t *body = get_blob(&r, &length);
  if (!reader_done(&r) || length != WIFI_EVENT_BYTES) {
    return fault(WIO_NETWORK_PROTOCOL);
  }
  wio_network_event_t event = {0};
  event.id = read_le32(body);
  if (event.id > 25U || ((event.id == 4U || event.id == 5U) && body[36] > 32U)) {
    return fault(WIO_NETWORK_PROTOCOL);
  }
  if (event.id == 5U) { event.disconnect_reason = body[43]; }
  if (event.id == 7U) { memcpy(event.address.octet, body + 4U, 4U); }
  ++network.diagnostics.event_count;
  if (network.callback != NULL) { network.callback(&event, network.callback_context); }
  reset_parser();
  return WIO_NETWORK_OK;
}

static wio_network_result_t rpc(DEADLINE *deadline, READER *reply) {
  if (!network.tx_valid) { return WIO_NETWORK_STATE; }
  if (deadline_expired(deadline)) { return WIO_NETWORK_TIMEOUT; }
  const uint32_t request_id = read_le32(network.tx + 4U);
  const uint32_t sequence = read_le32(network.tx + 8U);
  size_t length = network.tx_used - 4U;
  uint16_t crc = payload_crc(network.tx + 4U, length);
  network.tx[0] = (uint8_t)length;
  network.tx[1] = (uint8_t)(length >> 8);
  network.tx[2] = (uint8_t)crc;
  network.tx[3] = (uint8_t)(crc >> 8);
  if (!wio_network_port_send(network.tx, network.tx_used)) { return fault(WIO_NETWORK_IO); }
  ++network.diagnostics.rpc_count;
  /* The persistent RX ring remains armed while TX drains. A response cannot
   * be accepted and the TX buffer reused until the final stop bit has left.
   */
  while (!wio_network_port_tx_done()) {
    if (wio_network_port_error()) { return fault(WIO_NETWORK_IO); }
    if (deadline_expired(deadline)) { return fault(WIO_NETWORK_TIMEOUT); }
    wio_network_port_idle();
  }
  for (;;) {
    if (deadline_expired(deadline)) { return fault(WIO_NETWORK_TIMEOUT); }
    int received = receive_frame();
    if (received < 0) { return (wio_network_result_t)received; }
    /* Ring copying and CRC calculation are part of the same total deadline. */
    if (deadline_expired(deadline)) { return fault(WIO_NETWORK_TIMEOUT); }
    if (received == 1) {
      uint32_t message = read_le32(network.rx);
      if ((message & 0xFFU) == ERPC_ONEWAY) {
        wio_network_result_t result = event_frame();
        if (result != WIO_NETWORK_OK) { return result; }
      } else {
        if (message != (request_id | ERPC_REPLY) ||
            read_le32(network.rx + 4U) != sequence) {
          return fault(WIO_NETWORK_PROTOCOL);
        }
        *reply = (READER){network.rx, network.rx_length, 8U, true};
        reset_parser();
        return wio_network_port_error() ? fault(WIO_NETWORK_IO) : WIO_NETWORK_OK;
      }
    }
    wio_network_port_idle();
  }
}
static wio_network_result_t reply_i32(DEADLINE *deadline, int32_t *value) {
  READER reply;
  wio_network_result_t result = rpc(deadline, &reply);
  if (result != WIO_NETWORK_OK) { return result; }
  *value = (int32_t)get_u32(&reply);
  return reader_done(&reply) ? WIO_NETWORK_OK : fault(WIO_NETWORK_PROTOCOL);
}
static wio_network_result_t scalar_rpc(DEADLINE *deadline, uint8_t service,
                                       uint8_t method, bool has_arg,
                                       uint32_t arg, int32_t *value) {
  request_start(service, method);
  if (has_arg) { put_u32(arg); }
  return reply_i32(deadline, value);
}
static wio_network_result_t remote_result(int32_t value) {
  network.diagnostics.remote_result = value;
  return value == 0 ? WIO_NETWORK_OK : WIO_NETWORK_REMOTE;
}
static wio_network_result_t scalar_ok(DEADLINE *d, uint8_t service, uint8_t method,
                                      bool has_arg, uint32_t arg) {
  int32_t value;
  wio_network_result_t result = scalar_rpc(d, service, method, has_arg, arg, &value);
  return result == WIO_NETWORK_OK ? remote_result(value) : result;
}
static wio_network_result_t ensure_tcpip(DEADLINE *d) {
  if (network.tcpip_initialized) { return WIO_NETWORK_OK; }
  wio_network_result_t result = scalar_ok(d, SERVICE_TCPIP, 1U, false, 0U);
  if (result == WIO_NETWORK_OK) { network.tcpip_initialized = true; }
  return result;
}
static bool any_socket(void) {
  for (size_t i = 0U; i < WIO_NETWORK_MAX_SOCKETS; ++i) {
    if (network.sockets[i].handle != WIO_NETWORK_INVALID_SOCKET) { return true; }
  }
  return false;
}
static SOCKET *find_socket(wio_network_socket_t handle) {
  if (handle == WIO_NETWORK_INVALID_SOCKET) { return NULL; }
  for (size_t i = 0U; i < WIO_NETWORK_MAX_SOCKETS; ++i) {
    if (network.sockets[i].handle == handle) { return &network.sockets[i]; }
  }
  return NULL;
}
static wio_network_result_t socket_error(DEADLINE *d, int32_t value) {
  if (value != -1) { return fault(WIO_NETWORK_PROTOCOL); }
  int32_t remote_errno;
  wio_network_result_t result = scalar_rpc(d, SERVICE_SOCKET, 24U, false, 0U,
                                          &remote_errno);
  network.diagnostics.remote_result = value;
  if (result != WIO_NETWORK_OK) { return result; }
  if (remote_errno < 0 || remote_errno > 4095) { return fault(WIO_NETWORK_PROTOCOL); }
  network.diagnostics.remote_errno = remote_errno;
  return remote_errno == 11 ? WIO_NETWORK_WOULD_BLOCK : WIO_NETWORK_REMOTE;
}
static void put_endpoint(const wio_network_endpoint_t *endpoint) {
  uint8_t address[16] = {16U, 2U, 0U, 0U};
  address[2] = (uint8_t)(endpoint->port >> 8);
  address[3] = (uint8_t)endpoint->port;
  memcpy(address + 4U, endpoint->address.octet, 4U);
  put_blob(address, sizeof(address));
}

wio_network_state_t wio_network_get_state(void) { return network.diagnostics.state; }
wio_network_diagnostics_t wio_network_get_diagnostics(void) { return network.diagnostics; }

wio_network_result_t wio_network_init(wio_network_info_t *info,
                                      wio_network_callback_t callback,
                                      void *context, uint32_t timeout_ms) {
  if (info == NULL || !timeout_valid(timeout_ms)) { return WIO_NETWORK_ARGUMENT; }
  if (!wio_network_port_thread()) { return WIO_NETWORK_STATE; }
  if (network.busy) { return WIO_NETWORK_BUSY; }
  if (network.diagnostics.state == WIO_NETWORK_READY) { return WIO_NETWORK_STATE; }
  memset(info, 0, sizeof(*info));
  memset(&network, 0, sizeof(network));
  network.busy = true;
  network.callback = callback;
  network.callback_context = context;
  DEADLINE deadline;
  deadline_start(&deadline, timeout_ms);
  if (!wio_network_port_open()) { return finish(fault(WIO_NETWORK_IO)); }
  READER reply;
  request_start(SERVICE_SYSTEM, 1U);
  wio_network_result_t result = rpc(&deadline, &reply);
  if (result == WIO_NETWORK_OK) {
    size_t length;
    const uint8_t *version = get_blob(&reply, &length);
    if (!reader_done(&reply) || length == 0U || length >= sizeof(info->firmware)) {
      result = fault(WIO_NETWORK_PROTOCOL);
    } else {
      for (size_t i = 0U; i < length; ++i) {
        if (version[i] < 0x20U || version[i] > 0x7EU) {
          result = fault(WIO_NETWORK_PROTOCOL);
          break;
        }
      }
      if (result == WIO_NETWORK_OK) {
        memcpy(info->firmware, version, length);
        if (strcmp(info->firmware, WIO_NETWORK_FIRMWARE_VERSION) != 0) {
          result = fault(WIO_NETWORK_FIRMWARE);
        }
      }
    }
  }
  if (result == WIO_NETWORK_OK) {
    request_start(SERVICE_SYSTEM, 2U);
    put_u8(0xA5U);
    result = rpc(&deadline, &reply);
    if (result == WIO_NETWORK_OK &&
        (get_u8(&reply) != 0xA5U || !reader_done(&reply))) {
      result = fault(WIO_NETWORK_PROTOCOL);
    }
  }
  if (result == WIO_NETWORK_OK) {
    info->uart_baud = 614400U;
    info->max_transfer = WIO_NETWORK_MAX_TRANSFER;
    info->max_sockets = WIO_NETWORK_MAX_SOCKETS;
    info->echo_ok = true;
    network.diagnostics.state = WIO_NETWORK_READY;
  } else if (network.diagnostics.state != WIO_NETWORK_FAULTED) {
    result = fault(result);
  }
  return finish(result);
}

wio_network_result_t wio_network_deinit(void) {
  if (!wio_network_port_thread()) { return WIO_NETWORK_STATE; }
  if (network.busy) { return WIO_NETWORK_BUSY; }
  wio_network_port_close();
  memset(network.sockets, 0, sizeof(network.sockets));
  network.tcpip_initialized = false;
  network.callback = NULL;
  network.callback_context = NULL;
  network.diagnostics.state = WIO_NETWORK_OFF;
  reset_parser();
  return finish(WIO_NETWORK_OK);
}

wio_network_result_t wio_network_poll(uint32_t timeout_ms) {
  if (timeout_ms > WIO_NETWORK_MAX_TIMEOUT_MS) { return WIO_NETWORK_ARGUMENT; }
  DEADLINE deadline;
  wio_network_result_t result = begin(&deadline, timeout_ms == 0U ? 1U : timeout_ms);
  if (result != WIO_NETWORK_OK) { return result; }
  unsigned frames = 0U;
  for (;;) {
    int received = receive_frame();
    if (received < 0) { return finish((wio_network_result_t)received); }
    if (received == 1) {
      result = event_frame();
      if (result != WIO_NETWORK_OK) { return finish(result); }
      ++frames;
    }
    if (timeout_ms == 0U ? (received == 0 || frames >= 8U) : deadline_expired(&deadline)) {
      return finish(WIO_NETWORK_OK);
    }
    wio_network_port_idle();
  }
}

static wio_network_result_t wait_interval(DEADLINE *deadline, uint32_t ms) {
  uint32_t start = wio_network_port_millis();
  while ((uint32_t)(wio_network_port_millis() - start) < ms) {
    if (deadline_expired(deadline)) { return WIO_NETWORK_TIMEOUT; }
    int received = receive_frame();
    if (received < 0) { return (wio_network_result_t)received; }
    if (received == 1) {
      wio_network_result_t result = event_frame();
      if (result != WIO_NETWORK_OK) { return result; }
    }
    wio_network_port_idle();
  }
  return WIO_NETWORK_OK;
}

wio_network_result_t wio_network_join(const char *ssid, const char *password,
                                      wio_network_security_t security,
                                      uint32_t timeout_ms) {
  size_t ssid_length = bounded_length(ssid, 32U);
  size_t password_length = password == NULL ? 0U : bounded_length(password, 63U);
  if (ssid_length == 0U || ssid_length > 32U ||
      (security != WIO_NETWORK_SECURITY_OPEN && security != WIO_NETWORK_SECURITY_WPA2_AES_PSK) ||
      (security == WIO_NETWORK_SECURITY_OPEN ? password_length != 0U :
       password_length < 8U || password_length > 63U)) { return WIO_NETWORK_ARGUMENT; }
  for (size_t i = 0U; i < password_length; ++i) {
    if ((uint8_t)password[i] < 0x20U || (uint8_t)password[i] > 0x7EU) {
      return WIO_NETWORK_ARGUMENT;
    }
  }
  DEADLINE deadline;
  wio_network_result_t result = begin(&deadline, timeout_ms);
  if (result != WIO_NETWORK_OK) { return result; }
  if (any_socket()) { return finish(WIO_NETWORK_BUSY); }
  result = ensure_tcpip(&deadline);
  if (result != WIO_NETWORK_OK) { return finish(result); }
  int32_t value;
  /* DHCP may already be stopped. The reference shim maps that state to -1. */
  result = scalar_rpc(&deadline, SERVICE_TCPIP, 14U, true, 0U, &value);
  if (result != WIO_NETWORK_OK) { return finish(result); }
  if (value != 0 && value != -1) { return finish(remote_result(value)); }
  result = scalar_ok(&deadline, SERVICE_WIFI, 28U, false, 0U);
  if (result == WIO_NETWORK_OK) { result = wait_interval(&deadline, 20U); }
  if (result == WIO_NETWORK_OK) { result = scalar_ok(&deadline, SERVICE_WIFI, 27U, true, 1U); }
  if (result != WIO_NETWORK_OK) { return finish(result); }
  request_start(SERVICE_WIFI, 47U);
  put_u8(0U); /* Explicit join requests no background automatic reconnect. */
  result = reply_i32(&deadline, &value);
  if (result != WIO_NETWORK_OK) { return finish(result); }
  if (value != 0) { return finish(remote_result(value)); }
  request_start(SERVICE_WIFI, 1U);
  put_blob(ssid, ssid_length);
  put_u8(security == WIO_NETWORK_SECURITY_OPEN ? 1U : 0U);
  if (security != WIO_NETWORK_SECURITY_OPEN) { put_blob(password, password_length); }
  put_u32(security == WIO_NETWORK_SECURITY_OPEN ? 0U : 0x00400004U);
  put_u32(UINT32_MAX); /* key_id = -1, not WEP. */
  put_u32(0U);        /* semaphore = NULL: remote call is synchronous. */
  result = reply_i32(&deadline, &value);
  /* Remove credentials before constructing the next request as well as on exit. */
  scrub(network.tx, sizeof(network.tx));
  if (result != WIO_NETWORK_OK) { return finish(result); }
  if (value != 0) { return finish(remote_result(value)); }
  result = scalar_ok(&deadline, SERVICE_TCPIP, 13U, true, 0U);
  return finish(result);
}

wio_network_result_t wio_network_disconnect(uint32_t timeout_ms) {
  DEADLINE deadline;
  wio_network_result_t result = begin(&deadline, timeout_ms);
  if (result != WIO_NETWORK_OK) { return result; }
  if (any_socket()) { return finish(WIO_NETWORK_BUSY); }
  result = scalar_ok(&deadline, SERVICE_WIFI, 3U, false, 0U);
  if (result == WIO_NETWORK_OK && network.tcpip_initialized) {
    int32_t value;
    result = scalar_rpc(&deadline, SERVICE_TCPIP, 14U, true, 0U, &value);
    if (result == WIO_NETWORK_OK && value != 0 && value != -1) { result = remote_result(value); }
  }
  return finish(result);
}

wio_network_result_t wio_network_get_status(wio_network_status_t *status,
                                            uint32_t timeout_ms) {
  if (status == NULL) { return WIO_NETWORK_ARGUMENT; }
  memset(status, 0, sizeof(*status));
  DEADLINE deadline;
  wio_network_result_t result = begin(&deadline, timeout_ms);
  if (result != WIO_NETWORK_OK) { return result; }
  int32_t value;
  result = scalar_rpc(&deadline, SERVICE_WIFI, 4U, false, 0U, &value);
  if (result != WIO_NETWORK_OK) { return finish(result); }
  if (value != 0) { return finish(WIO_NETWORK_OK); }
  request_start(SERVICE_TCPIP, 7U);
  put_u32(0U);
  READER reply;
  result = rpc(&deadline, &reply);
  if (result != WIO_NETWORK_OK) { return finish(result); }
  size_t length;
  const uint8_t *ip_info = get_blob(&reply, &length);
  value = (int32_t)get_u32(&reply);
  if (!reader_done(&reply) || length != 12U) { return finish(fault(WIO_NETWORK_PROTOCOL)); }
  if (value != 0) { return finish(remote_result(value)); }
  status->associated = true;
  memcpy(status->address.octet, ip_info, 4U);
  memcpy(status->netmask.octet, ip_info + 4U, 4U);
  memcpy(status->gateway.octet, ip_info + 8U, 4U);
  status->has_ipv4 = (ip_info[0] | ip_info[1] | ip_info[2] | ip_info[3]) != 0U;
  return finish(WIO_NETWORK_OK);
}

wio_network_result_t wio_network_resolve(const char *hostname,
                                         wio_network_ipv4_t *address,
                                         uint32_t timeout_ms) {
  size_t length = bounded_length(hostname, 253U);
  if (address == NULL || length == 0U || length > 253U) { return WIO_NETWORK_ARGUMENT; }
  for (size_t i = 0U; i < length; ++i) {
    uint8_t c = (uint8_t)hostname[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '.')) { return WIO_NETWORK_ARGUMENT; }
  }
  memset(address, 0, sizeof(*address));
  DEADLINE deadline;
  wio_network_result_t result = begin(&deadline, timeout_ms);
  if (result != WIO_NETWORK_OK) { return result; }
  result = ensure_tcpip(&deadline);
  if (result != WIO_NETWORK_OK) { return finish(result); }
  request_start(SERVICE_SOCKET, 25U);
  put_blob(hostname, length);
  READER reply;
  result = rpc(&deadline, &reply);
  if (result != WIO_NETWORK_OK) { return finish(result); }
  size_t address_length;
  const uint8_t *bytes = get_blob(&reply, &address_length);
  int32_t error = (int8_t)get_u8(&reply);
  /* Default pinned RTL core has LWIP_IPV6=0: ip_addr_t is exactly four bytes.
   * A differently configured 2.1.3 binary is not silently treated as compatible.
   */
  if (!reader_done(&reply) || address_length != 4U) { return finish(fault(WIO_NETWORK_PROTOCOL)); }
  if (error != 0) { return finish(remote_result(error)); }
  memcpy(address->octet, bytes, 4U);
  return finish(WIO_NETWORK_OK);
}

wio_network_result_t wio_network_socket_open(wio_network_socket_type_t type,
                                            wio_network_socket_t *socket,
                                            uint32_t timeout_ms) {
  if (socket == NULL || (type != WIO_NETWORK_TCP && type != WIO_NETWORK_UDP)) {
    return WIO_NETWORK_ARGUMENT;
  }
  *socket = WIO_NETWORK_INVALID_SOCKET;
  DEADLINE deadline;
  wio_network_result_t result = begin(&deadline, timeout_ms);
  if (result != WIO_NETWORK_OK) { return result; }
  SOCKET *slot = NULL;
  for (size_t i = 0U; i < WIO_NETWORK_MAX_SOCKETS; ++i) {
    if (network.sockets[i].handle == WIO_NETWORK_INVALID_SOCKET) { slot = &network.sockets[i]; break; }
  }
  if (slot == NULL || last_handle == UINT32_MAX) { return finish(WIO_NETWORK_NO_SOCKET); }
  result = ensure_tcpip(&deadline);
  if (result != WIO_NETWORK_OK) { return finish(result); }
  request_start(SERVICE_SOCKET, 18U);
  put_u32(2U); /* AF_INET */
  put_u32(type == WIO_NETWORK_TCP ? 1U : 2U);
  put_u32(type == WIO_NETWORK_TCP ? 6U : 17U);
  int32_t value;
  result = reply_i32(&deadline, &value);
  if (result != WIO_NETWORK_OK) { return finish(result); }
  if (value < 0) { return finish(socket_error(&deadline, value)); }
  for (size_t i = 0U; i < WIO_NETWORK_MAX_SOCKETS; ++i) {
    if (network.sockets[i].handle != 0U && network.sockets[i].descriptor == value) {
      return finish(fault(WIO_NETWORK_PROTOCOL));
    }
  }
  *slot = (SOCKET){++last_handle, value, type, false};
  *socket = slot->handle;
  return finish(WIO_NETWORK_OK);
}

static wio_network_result_t socket_address_call(wio_network_socket_t handle,
                                                const wio_network_endpoint_t *endpoint,
                                                uint32_t timeout_ms, bool connect) {
  if (endpoint == NULL || (connect && endpoint->port == 0U)) { return WIO_NETWORK_ARGUMENT; }
  DEADLINE deadline;
  wio_network_result_t result = begin(&deadline, timeout_ms);
  if (result != WIO_NETWORK_OK) { return result; }
  SOCKET *socket = find_socket(handle);
  if (socket == NULL || socket->connected) { return finish(WIO_NETWORK_STATE); }
  request_start(SERVICE_SOCKET, connect ? 9U : 2U);
  put_u32((uint32_t)socket->descriptor);
  put_endpoint(endpoint);
  put_u32(16U);
  int32_t value;
  result = reply_i32(&deadline, &value);
  if (result != WIO_NETWORK_OK) { return finish(result); }
  if (value < 0) { return finish(socket_error(&deadline, value)); }
  if (value != 0) { return finish(fault(WIO_NETWORK_PROTOCOL)); }
  if (connect) { socket->connected = true; }
  return finish(WIO_NETWORK_OK);
}
wio_network_result_t wio_network_socket_connect(wio_network_socket_t socket,
                                               const wio_network_endpoint_t *peer,
                                               uint32_t timeout_ms) {
  return socket_address_call(socket, peer, timeout_ms, true);
}
wio_network_result_t wio_network_socket_bind(wio_network_socket_t socket,
                                            const wio_network_endpoint_t *local,
                                            uint32_t timeout_ms) {
  return socket_address_call(socket, local, timeout_ms, false);
}

static wio_network_result_t socket_send_data(wio_network_socket_t handle,
                                             const void *data, size_t length,
                                             const wio_network_endpoint_t *peer,
                                             size_t *sent, uint32_t timeout_ms) {
  if (sent == NULL || data == NULL || length == 0U ||
      length > WIO_NETWORK_MAX_TRANSFER || (peer != NULL && peer->port == 0U)) {
    return WIO_NETWORK_ARGUMENT;
  }
  *sent = 0U;
  DEADLINE deadline;
  wio_network_result_t result = begin(&deadline, timeout_ms);
  if (result != WIO_NETWORK_OK) { return result; }
  SOCKET *socket = find_socket(handle);
  if (socket == NULL || (peer != NULL && socket->type != WIO_NETWORK_UDP) ||
      (peer == NULL && !socket->connected)) { return finish(WIO_NETWORK_STATE); }
  request_start(SERVICE_SOCKET, peer == NULL ? 15U : 17U);
  put_u32((uint32_t)socket->descriptor);
  put_blob(data, length);
  put_u32(LWIP_MSG_DONTWAIT);
  if (peer != NULL) { put_endpoint(peer); put_u32(16U); }
  int32_t value;
  result = reply_i32(&deadline, &value);
  if (result != WIO_NETWORK_OK) { return finish(result); }
  if (value < 0) { return finish(socket_error(&deadline, value)); }
  if ((uint32_t)value > length || (socket->type == WIO_NETWORK_UDP && (uint32_t)value != length)) {
    return finish(fault(WIO_NETWORK_PROTOCOL));
  }
  *sent = (size_t)value;
  return finish(WIO_NETWORK_OK);
}
wio_network_result_t wio_network_socket_send(wio_network_socket_t socket,
                                            const void *data, size_t length,
                                            size_t *sent, uint32_t timeout_ms) {
  return socket_send_data(socket, data, length, NULL, sent, timeout_ms);
}
wio_network_result_t wio_network_socket_sendto(wio_network_socket_t socket,
                                              const void *data, size_t length,
                                              const wio_network_endpoint_t *peer,
                                              size_t *sent, uint32_t timeout_ms) {
  if (peer == NULL) { return WIO_NETWORK_ARGUMENT; }
  return socket_send_data(socket, data, length, peer, sent, timeout_ms);
}

static wio_network_result_t socket_receive_data(wio_network_socket_t handle,
                                                void *data, size_t capacity,
                                                wio_network_endpoint_t *peer,
                                                size_t *received, uint32_t timeout_ms) {
  if (data == NULL || received == NULL || capacity == 0U || capacity > WIO_NETWORK_MAX_TRANSFER) {
    return WIO_NETWORK_ARGUMENT;
  }
  *received = 0U;
  if (peer != NULL) { memset(peer, 0, sizeof(*peer)); }
  DEADLINE deadline;
  wio_network_result_t result = begin(&deadline, timeout_ms);
  if (result != WIO_NETWORK_OK) { return result; }
  SOCKET *socket = find_socket(handle);
  if (socket == NULL || (peer != NULL && socket->type != WIO_NETWORK_UDP) ||
      (socket->type == WIO_NETWORK_TCP && !socket->connected)) { return finish(WIO_NETWORK_STATE); }
  request_start(SERVICE_SOCKET, peer == NULL ? 12U : 14U);
  put_u32((uint32_t)socket->descriptor);
  put_u32((uint32_t)capacity);
  put_u32(LWIP_MSG_DONTWAIT);
  if (peer != NULL) { put_u32(16U); }
  put_u32(0U); /* Firmware ignores this argument; DONTWAIT makes it nonblocking. */
  READER reply;
  result = rpc(&deadline, &reply);
  if (result != WIO_NETWORK_OK) { return finish(result); }
  size_t length, address_length = 0U;
  const uint8_t *bytes = get_blob(&reply, &length);
  const uint8_t *address = NULL;
  uint32_t from_length = 0U;
  if (peer != NULL) {
    address = get_blob(&reply, &address_length);
    from_length = get_u32(&reply);
  }
  int32_t value = (int32_t)get_u32(&reply);
  if (!reader_done(&reply) ||
      (value > 0 ? ((uint32_t)value > capacity || length != (uint32_t)value) : length != 1U) ||
      (peer != NULL && address_length != 16U)) { return finish(fault(WIO_NETWORK_PROTOCOL)); }
  /* On EOF/error the original firmware serializes one uninitialized dummy
   * byte. Discard it; it is never valid application data.
   */
  if (value < 0) { return finish(socket_error(&deadline, value)); }
  if (peer != NULL) {
    if (from_length != 16U || address[0] != 16U || address[1] != 2U) {
      return finish(fault(WIO_NETWORK_PROTOCOL));
    }
    memcpy(peer->address.octet, address + 4U, 4U);
    peer->port = (uint16_t)((uint16_t)address[2] << 8 | address[3]);
  }
  if (value > 0) { memcpy(data, bytes, (size_t)value); }
  *received = (size_t)value;
  return finish(WIO_NETWORK_OK);
}
wio_network_result_t wio_network_socket_receive(wio_network_socket_t socket,
                                               void *data, size_t capacity,
                                               size_t *received, uint32_t timeout_ms) {
  return socket_receive_data(socket, data, capacity, NULL, received, timeout_ms);
}
wio_network_result_t wio_network_socket_recvfrom(wio_network_socket_t socket,
                                                void *data, size_t capacity,
                                                wio_network_endpoint_t *peer,
                                                size_t *received, uint32_t timeout_ms) {
  if (peer == NULL) { return WIO_NETWORK_ARGUMENT; }
  return socket_receive_data(socket, data, capacity, peer, received, timeout_ms);
}
wio_network_result_t wio_network_socket_close(wio_network_socket_t handle,
                                             uint32_t timeout_ms) {
  DEADLINE deadline;
  wio_network_result_t result = begin(&deadline, timeout_ms);
  if (result != WIO_NETWORK_OK) { return result; }
  SOCKET *socket = find_socket(handle);
  if (socket == NULL) { return finish(WIO_NETWORK_STATE); }
  int32_t value;
  result = scalar_rpc(&deadline, SERVICE_SOCKET, 8U, true,
                      (uint32_t)socket->descriptor, &value);
  if (result != WIO_NETWORK_OK) { return finish(result); }
  if (value < 0) { return finish(socket_error(&deadline, value)); }
  if (value != 0) { return finish(fault(WIO_NETWORK_PROTOCOL)); }
  memset(socket, 0, sizeof(*socket));
  return finish(WIO_NETWORK_OK);
}
