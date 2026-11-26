/* SPDX-License-Identifier: Apache-2.0 */
/* Host-only peer simulation. No network, serial, hardware, credentials or radio. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "wio_network.h"
#include "wio_network_port.h"

enum fault_mode {
  NORMAL, NO_REPLY, SHORT_HEADER, SHORT_PAYLOAD, LENGTH_SMALL, LENGTH_LARGE,
  BAD_CRC, BAD_SEQUENCE, BAD_SERVICE, BAD_METHOD, BAD_VERSION, BAD_TYPE,
  EXTRA_BYTE, BAD_STRING_LENGTH, EMBEDDED_NUL, FIRMWARE_OTHER, RX_LOSS,
  RX_LOSS_AFTER_READ, ECHO_WRONG, EVENT_UNKNOWN, EVENT_SHORT, DNS_LONG,
  RANDOM_RESPONSE, DEADLINE_AFTER_READ, EVENT_STORM
};
static struct {
  uint8_t rx[65536], first_tx[4100], scratch[5000];
  size_t used, consumed, chunk, first_length;
  uint32_t clock, sent, opens, closes, previous_sequence;
  uint32_t services[256], methods[256], callback_count;
  bool open, error, thread, frozen, event_before_reply, associated;
  bool lose_after_read, tx_pending;
  bool advance_after_read, event_storm;
  uint32_t tx_until;
  const uint8_t *tx_pointer;
  size_t tx_length;
  enum fault_mode fault;
  uint8_t fault_service, fault_method;
  int32_t next_fd, force_fd, recv_result, send_result, dns_error, remote_errno;
  bool forced_fd, recv_eof;
  uint32_t random_seed;
} peer;
static unsigned assertions;
#define CHECK(expression) do { ++assertions; assert(expression); } while (0)

static uint32_t le32(const uint8_t *p) {
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void u32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value; p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16); p[3] = (uint8_t)(value >> 24);
}
static uint16_t crc(const uint8_t *p, size_t n) {
  uint32_t value = 0xEF4AU;
  while (n-- != 0U) {
    value ^= (uint32_t)*p++ << 8;
    for (unsigned b = 0U; b < 8U; ++b) {
      value = (value << 1) ^ ((value & 0x8000U) != 0U ? 0x1021U : 0U);
      value &= 0xFFFFU;
    }
  }
  return (uint16_t)value;
}
static void queue(const uint8_t *bytes, size_t length) {
  CHECK(peer.used + length <= sizeof(peer.rx));
  memcpy(peer.rx + peer.used, bytes, length);
  peer.used += length;
}
static void queue_body(const uint8_t *body, size_t length) {
  uint16_t checksum = crc(body, length);
  uint8_t header[4] = {(uint8_t)length, (uint8_t)(length >> 8),
                       (uint8_t)checksum, (uint8_t)(checksum >> 8)};
  queue(header, sizeof(header));
  queue(body, length);
}
static void event(uint32_t id) {
  uint8_t body[60] = {1U, 1U, 19U, 1U};
  u32(body + 4, 0x12345678U);
  u32(body + 8, 48U);
  u32(body + 12, id);
  body[16] = 192U; body[17] = 0U; body[18] = 2U; body[19] = 42U;
  if (peer.fault == EVENT_SHORT) { u32(body + 8, 47U); }
  queue_body(body, sizeof(body));
}
static void callback(const wio_network_event_t *ev, void *context) {
  CHECK(context == &peer);
  CHECK(ev->id == 7U);
  CHECK(ev->address.octet[0] == 192U && ev->address.octet[3] == 42U);
  CHECK(wio_network_deinit() == WIO_NETWORK_BUSY);
  CHECK(wio_network_poll(0U) == WIO_NETWORK_BUSY);
  ++peer.callback_count;
}

bool wio_network_port_thread(void) { return peer.thread; }
uint32_t wio_network_port_millis(void) { return peer.clock; }
void wio_network_port_idle(void) { if (!peer.frozen) { ++peer.clock; } }
bool wio_network_port_open(void) { CHECK(!peer.open); peer.open = true; ++peer.opens; return true; }
void wio_network_port_close(void) { peer.open = false; peer.tx_pending = false; ++peer.closes; }
bool wio_network_port_error(void) { return peer.error || !peer.open; }
int32_t wio_network_port_read(uint8_t *data, size_t capacity) {
  if (peer.error) { return -1; }
  size_t count = peer.used - peer.consumed;
  if (count > capacity) { count = capacity; }
  if (count > peer.chunk) { count = peer.chunk; }
  memcpy(data, peer.rx + peer.consumed, count);
  peer.consumed += count;
  if (peer.lose_after_read && peer.consumed == peer.used) { peer.error = true; }
  if (peer.advance_after_read && peer.consumed == peer.used) { peer.clock += 5000U; }
  if (peer.event_storm && peer.consumed == peer.used) {
    peer.consumed = 0U; peer.used = 0U; event(7U);
  }
  return (int32_t)count;
}
bool wio_network_port_tx_done(void) {
  if ((int32_t)(peer.clock - peer.tx_until) < 0) { return false; }
  if (peer.tx_pending) {
    CHECK(peer.tx_pointer[0] == (uint8_t)(peer.tx_length - 4U));
    CHECK(crc(peer.tx_pointer + 4U, peer.tx_length - 4U) ==
          (uint16_t)((uint16_t)peer.tx_pointer[2] | (uint16_t)peer.tx_pointer[3] << 8));
  }
  peer.tx_pending = false;
  return true;
}

bool wio_network_port_send(const uint8_t *data, size_t length) {
  CHECK(peer.open && !peer.tx_pending);
  CHECK(length >= 12U && length <= 4100U);
  CHECK(((size_t)data[0] | (size_t)data[1] << 8) == length - 4U);
  CHECK(crc(data + 4U, length - 4U) == (uint16_t)((uint16_t)data[2] | (uint16_t)data[3] << 8));
  CHECK(data[4] == 0U && data[7] == 1U);
  uint32_t sequence = le32(data + 8U);
  CHECK(sequence > peer.previous_sequence);
  peer.previous_sequence = sequence;
  peer.tx_pointer = data;
  peer.tx_length = length;
  peer.tx_pending = true;
  peer.tx_until = peer.clock + (peer.frozen ? 0U : 2U);
  if (peer.first_length == 0U) { memcpy(peer.first_tx, data, length); peer.first_length = length; }
  uint8_t service = data[6], method = data[5];
  CHECK(peer.sent < 256U);
  peer.services[peer.sent] = service;
  peer.methods[peer.sent++] = method;
  uint8_t body[5000] = {0};
  memcpy(body, data + 4U, 8U);
  body[0] = 2U;
  size_t body_length = 12U;
  int32_t result = 0;
  if (service == 1U && method == 1U) {
    CHECK(length == 12U);
    u32(body + 8U, 5U);
    memcpy(body + 12U, "2.1.3", 5U);
    body_length = 17U;
  } else if (service == 1U && method == 2U) {
    CHECK(length == 13U && data[12] == 0xA5U);
    body[8] = data[12]; body_length = 9U;
  } else if (service == 14U) {
    if (method == 4U) { result = peer.associated ? 0 : -1; }
    else if (method == 47U) { CHECK(length == 13U && data[12] == 0U); }
    else if (method == 27U) { CHECK(length == 16U && le32(data + 12U) == 1U); }
    else if (method == 1U) {
      size_t ssid_length = le32(data + 12U);
      CHECK(ssid_length >= 1U && ssid_length <= 32U);
      size_t offset = 16U + ssid_length;
      bool is_null = data[offset++] == 1U;
      if (!is_null) {
        size_t password_length = le32(data + offset); offset += 4U;
        CHECK(password_length >= 8U && password_length <= 63U);
        offset += password_length;
      }
      CHECK(le32(data + offset) == (is_null ? 0U : 0x00400004U)); offset += 4U;
      CHECK(le32(data + offset) == UINT32_MAX); offset += 4U;
      CHECK(le32(data + offset) == 0U); offset += 4U;
      CHECK(offset == length);
      peer.associated = true;
    } else { CHECK(method == 28U || method == 3U); if (method == 3U) { peer.associated = false; } }
    u32(body + 8U, (uint32_t)result);
  } else if (service == 15U) {
    CHECK(method == 1U || method == 7U || method == 13U || method == 14U);
    if (method == 7U) {
      u32(body + 8U, 12U);
      const uint8_t ip[12] = {192U,0U,2U,42U,255U,255U,255U,0U,192U,0U,2U,1U};
      memcpy(body + 12U, ip, 12U); u32(body + 24U, 0U); body_length = 28U;
    } else { u32(body + 8U, 0U); }
  } else if (service == 16U) {
    if (method == 18U) {
      CHECK(length == 24U && le32(data + 12U) == 2U);
      uint32_t type = le32(data + 16U);
      CHECK((type == 1U && le32(data + 20U) == 6U) ||
            (type == 2U && le32(data + 20U) == 17U));
      result = peer.forced_fd ? peer.force_fd : peer.next_fd++;
    } else if (method == 9U || method == 2U) {
      CHECK(length == 40U && le32(data + 16U) == 16U);
      CHECK(data[20] == 16U && data[21] == 2U && le32(data + 36U) == 16U);
    } else if (method == 15U || method == 17U) {
      size_t count = le32(data + 16U);
      CHECK(count <= WIO_NETWORK_MAX_TRANSFER);
      CHECK(le32(data + 20U + count) == 8U);
      CHECK(length == count + (method == 15U ? 24U : 48U));
      if (method == 17U) {
        CHECK(le32(data + 24U + count) == 16U);
        CHECK(data[28U + count] == 16U && data[29U + count] == 2U);
        CHECK(le32(data + 44U + count) == 16U);
      }
      result = peer.send_result == INT32_MAX ? (int32_t)count : peer.send_result;
    } else if (method == 12U || method == 14U) {
      CHECK(le32(data + 20U) == 8U);
      CHECK(length == (method == 12U ? 28U : 32U));
      CHECK(le32(data + length - 4U) == 0U);
      result = peer.recv_result;
      size_t count = result > 0 ? (size_t)result : 1U;
      CHECK(count < 4000U);
      u32(body + 8U, (uint32_t)count);
      for (size_t i = 0U; i < count; ++i) { body[12U + i] = result > 0 ? (uint8_t)('a' + i % 26U) : 0xE7U; }
      size_t offset = 12U + count;
      if (method == 14U) {
        CHECK(le32(data + 24U) == 16U);
        u32(body + offset, 16U); offset += 4U;
        const uint8_t address[16] = {16U,2U,0x12U,0x34U,198U,51U,100U,7U};
        memcpy(body + offset, address, sizeof(address)); offset += sizeof(address);
        u32(body + offset, 16U); offset += 4U;
      }
      u32(body + offset, (uint32_t)result); body_length = offset + 4U;
    } else if (method == 24U) { result = peer.remote_errno; }
    else if (method == 25U) {
      CHECK(le32(data + 12U) == length - 16U);
      u32(body + 8U, 4U);
      body[12] = 203U; body[13] = 0U; body[14] = 113U; body[15] = 9U;
      body[16] = (uint8_t)peer.dns_error;
      body_length = 17U;
    } else { CHECK(method == 8U); }
    if (method != 12U && method != 14U && method != 25U) { u32(body + 8U, (uint32_t)result); }
  } else { CHECK(false); }

  if (peer.event_before_reply) { event(7U); peer.event_before_reply = false; }
  enum fault_mode mode = service == peer.fault_service && method == peer.fault_method ? peer.fault : NORMAL;
  switch (mode) {
    case NO_REPLY: return true;
    case SHORT_HEADER: { const uint8_t h[] = {17U, 0U}; queue(h, sizeof(h)); return true; }
    case SHORT_PAYLOAD: { const uint8_t h[] = {17U, 0U, 0U, 0U, 2U, 1U}; queue(h, sizeof(h)); return true; }
    case LENGTH_SMALL: { const uint8_t h[] = {7U, 0U, 0U, 0U}; queue(h, sizeof(h)); return true; }
    case LENGTH_LARGE: { const uint8_t h[] = {1U, 16U, 0U, 0U}; queue(h, sizeof(h)); return true; }
    case BAD_SEQUENCE: u32(body + 4U, sequence - 1U); break;
    case BAD_SERVICE: body[2] ^= 1U; break;
    case BAD_METHOD: body[1] ^= 1U; break;
    case BAD_VERSION: body[3] = 2U; break;
    case BAD_TYPE: body[0] = 0U; break;
    case EXTRA_BYTE: body[body_length++] = 0U; break;
    case BAD_STRING_LENGTH: u32(body + 8U, UINT32_MAX); break;
    case EMBEDDED_NUL: body[14] = 0U; break;
    case FIRMWARE_OTHER: body[16] = '4'; break;
    case RX_LOSS: peer.error = true; break;
    case RX_LOSS_AFTER_READ: peer.lose_after_read = true; break;
    case DEADLINE_AFTER_READ: peer.advance_after_read = true; break;
    case EVENT_STORM: peer.event_storm = true; event(7U); return true;
    case ECHO_WRONG: body[8] ^= 1U; break;
    case EVENT_UNKNOWN: event(26U); break;
    case EVENT_SHORT: event(7U); break;
    case DNS_LONG: u32(body + 8U, 20U); memset(body + 12U, 0, 20U); body[32] = 0U; body_length = 33U; break;
    case RANDOM_RESPONSE:
      body_length = (peer.random_seed % 80U) + 8U;
      for (size_t i = 0U; i < body_length; ++i) {
        peer.random_seed = peer.random_seed * 1664525U + 1013904223U;
        body[i] = (uint8_t)(peer.random_seed >> 24);
      }
      break;
    default: break;
  }
  size_t before = peer.used;
  queue_body(body, body_length);
  if (mode == BAD_CRC) { peer.rx[before + 2U] ^= 1U; }
  return true;
}

static void reset_peer(void) {
  peer.thread = true;
  CHECK(wio_network_deinit() == WIO_NETWORK_OK);
  uint32_t previous = peer.previous_sequence;
  memset(&peer, 0, sizeof(peer));
  peer.previous_sequence = previous;
  peer.thread = true;
  peer.chunk = 4096U;
  peer.send_result = INT32_MAX;
  peer.recv_result = 3;
  peer.remote_errno = 11;
  peer.fault_service = 1U;
  peer.fault_method = 1U;
}
static void attach(void) {
  wio_network_info_t info;
  CHECK(wio_network_init(&info, callback, &peer, 2000U) == WIO_NETWORK_OK);
  CHECK(strcmp(info.firmware, "2.1.3") == 0 && info.echo_ok);
  CHECK(info.uart_baud == 614400U && info.max_sockets == 4U && info.max_transfer == 1024U);
  CHECK(peer.sent == 2U && peer.services[0] == 1U && peer.methods[0] == 1U && peer.methods[1] == 2U);
  CHECK(wio_network_get_state() == WIO_NETWORK_READY);
}
static wio_network_socket_t open_socket(wio_network_socket_type_t type) {
  wio_network_socket_t socket;
  CHECK(wio_network_socket_open(type, &socket, 2000U) == WIO_NETWORK_OK);
  CHECK(socket != 0U);
  return socket;
}
static const wio_network_endpoint_t endpoint = {{{192U, 0U, 2U, 1U}}, 1234U};

static void test_probe_and_arguments(void) {
  reset_peer();
  CHECK(crc((const uint8_t *)"123456789", 9U) == 0x89ACU);
  wio_network_info_t info;
  CHECK(wio_network_init(NULL, NULL, NULL, 10U) == WIO_NETWORK_ARGUMENT);
  CHECK(wio_network_init(&info, NULL, NULL, 0U) == WIO_NETWORK_ARGUMENT);
  CHECK(wio_network_join(NULL, NULL, WIO_NETWORK_SECURITY_OPEN, 10U) == WIO_NETWORK_ARGUMENT);
  CHECK(peer.opens == 0U && peer.sent == 0U);
  peer.event_before_reply = true;
  peer.chunk = 1U;
  attach();
  static const uint8_t version_request[] = {8U,0U,0x7DU,0xA6U,0U,1U,1U,1U,1U,0U,0U,0U};
  CHECK(peer.first_length == sizeof(version_request));
  CHECK(memcmp(peer.first_tx, version_request, sizeof(version_request)) == 0);
  CHECK(peer.callback_count == 1U);
  CHECK(wio_network_get_diagnostics().event_count == 1U);
  CHECK(wio_network_init(&info, NULL, NULL, 10U) == WIO_NETWORK_STATE);
  CHECK(wio_network_poll(0U) == WIO_NETWORK_OK);
  CHECK(wio_network_poll(WIO_NETWORK_MAX_TIMEOUT_MS + 1U) == WIO_NETWORK_ARGUMENT);
  peer.thread = false;
  CHECK(wio_network_poll(0U) == WIO_NETWORK_STATE);
  peer.thread = true;
  CHECK(wio_network_join("test", "short", WIO_NETWORK_SECURITY_WPA2_AES_PSK, 10U) == WIO_NETWORK_ARGUMENT);
  CHECK(wio_network_join("test", "secret", WIO_NETWORK_SECURITY_OPEN, 10U) == WIO_NETWORK_ARGUMENT);
}

static void test_faults(void) {
  const enum fault_mode modes[] = {NO_REPLY, SHORT_HEADER, SHORT_PAYLOAD, LENGTH_SMALL,
    LENGTH_LARGE, BAD_CRC, BAD_SEQUENCE, BAD_SERVICE, BAD_METHOD, BAD_VERSION,
    BAD_TYPE, EXTRA_BYTE, BAD_STRING_LENGTH, EMBEDDED_NUL, FIRMWARE_OTHER,
    RX_LOSS, RX_LOSS_AFTER_READ, ECHO_WRONG, EVENT_UNKNOWN, EVENT_SHORT,
    DEADLINE_AFTER_READ, EVENT_STORM};
  for (size_t i = 0U; i < sizeof(modes) / sizeof(modes[0]); ++i) {
    reset_peer();
    peer.fault = modes[i];
    if (modes[i] == ECHO_WRONG || modes[i] == DEADLINE_AFTER_READ) { peer.fault_method = 2U; }
    wio_network_info_t info;
    wio_network_result_t result = wio_network_init(&info, NULL, NULL, 1000U);
    wio_network_result_t expected = WIO_NETWORK_PROTOCOL;
    if (modes[i] == NO_REPLY || modes[i] == SHORT_HEADER || modes[i] == SHORT_PAYLOAD) { expected = WIO_NETWORK_TIMEOUT; }
    if (modes[i] == DEADLINE_AFTER_READ || modes[i] == EVENT_STORM) { expected = WIO_NETWORK_TIMEOUT; }
    if (modes[i] == RX_LOSS || modes[i] == RX_LOSS_AFTER_READ) { expected = WIO_NETWORK_IO; }
    if (modes[i] == FIRMWARE_OTHER) { expected = WIO_NETWORK_FIRMWARE; CHECK(strcmp(info.firmware, "2.1.4") == 0); }
    CHECK(result == expected);
    CHECK(wio_network_get_state() == WIO_NETWORK_FAULTED && !peer.open);
    CHECK(!info.echo_ok && wio_network_poll(0U) == WIO_NETWORK_STATE);
  }
  reset_peer();
  peer.frozen = true; peer.fault = NO_REPLY;
  wio_network_info_t info;
  CHECK(wio_network_init(&info, NULL, NULL, 20U) == WIO_NETWORK_TIMEOUT);
  CHECK(peer.clock == 0U && wio_network_get_state() == WIO_NETWORK_FAULTED);
  for (unsigned i = 1U; i <= 256U; ++i) {
    reset_peer(); peer.fault = RANDOM_RESPONSE; peer.random_seed = i;
    CHECK(wio_network_init(&info, NULL, NULL, 20U) != WIO_NETWORK_OK);
    CHECK(wio_network_get_state() == WIO_NETWORK_FAULTED);
  }
}

static void test_join_status_dns(void) {
  reset_peer(); attach();
  wio_network_status_t status;
  CHECK(wio_network_get_status(&status, 100U) == WIO_NETWORK_OK && !status.associated);
  CHECK(wio_network_join("test-only", "not-a-real-password", WIO_NETWORK_SECURITY_WPA2_AES_PSK, 2000U) == WIO_NETWORK_OK);
  const uint32_t services[] = {15U,15U,14U,14U,14U,14U,15U};
  const uint32_t methods[] = {1U,14U,28U,27U,47U,1U,13U};
  CHECK(memcmp(peer.services + 3U, services, sizeof(services)) == 0);
  CHECK(memcmp(peer.methods + 3U, methods, sizeof(methods)) == 0);
  for (size_t i = 0U; i < 4100U; ++i) { CHECK(peer.tx_pointer[i] == 0U); }
  CHECK(wio_network_get_status(&status, 100U) == WIO_NETWORK_OK);
  CHECK(status.associated && status.has_ipv4 && status.address.octet[3] == 42U && status.gateway.octet[3] == 1U);
  wio_network_ipv4_t address;
  CHECK(wio_network_resolve("example.invalid", &address, 100U) == WIO_NETWORK_OK);
  CHECK(address.octet[0] == 203U && address.octet[3] == 9U);
  peer.dns_error = -5;
  CHECK(wio_network_resolve("example.invalid", &address, 100U) == WIO_NETWORK_REMOTE);
  CHECK(address.octet[0] == 0U && wio_network_get_diagnostics().remote_result == -5);
  CHECK(wio_network_disconnect(100U) == WIO_NETWORK_OK && !peer.associated);
  CHECK(wio_network_join("test-only", NULL, WIO_NETWORK_SECURITY_OPEN, 2000U) == WIO_NETWORK_OK);
  peer.fault = DNS_LONG; peer.fault_service = 16U; peer.fault_method = 25U;
  CHECK(wio_network_resolve("example.invalid", &address, 100U) == WIO_NETWORK_PROTOCOL);
  CHECK(wio_network_get_state() == WIO_NETWORK_FAULTED);
}

static void test_tcp(void) {
  reset_peer(); attach();
  wio_network_socket_t socket = open_socket(WIO_NETWORK_TCP);
  size_t count = 99U;
  uint8_t data[32]; memset(data, 0xCC, sizeof(data));
  CHECK(wio_network_socket_send(socket, "abcd", 4U, &count, 100U) == WIO_NETWORK_STATE && count == 0U);
  CHECK(wio_network_join("test-only", NULL, WIO_NETWORK_SECURITY_OPEN, 100U) == WIO_NETWORK_BUSY);
  CHECK(wio_network_disconnect(100U) == WIO_NETWORK_BUSY);
  CHECK(wio_network_socket_connect(socket, &endpoint, 100U) == WIO_NETWORK_OK);
  peer.send_result = 2;
  uint32_t before = peer.sent;
  CHECK(wio_network_socket_send(socket, "abcd", 4U, &count, 100U) == WIO_NETWORK_OK && count == 2U);
  CHECK(peer.sent == before + 1U);
  CHECK(wio_network_socket_receive(socket, data, sizeof(data), &count, 100U) == WIO_NETWORK_OK);
  CHECK(count == 3U && memcmp(data, "abc", 3U) == 0 && data[3] == 0xCCU);
  peer.recv_result = 0; memset(data, 0xCC, sizeof(data));
  CHECK(wio_network_socket_receive(socket, data, sizeof(data), &count, 100U) == WIO_NETWORK_OK && count == 0U);
  CHECK(data[0] == 0xCCU);
  peer.recv_result = -1;
  CHECK(wio_network_socket_receive(socket, data, sizeof(data), &count, 100U) == WIO_NETWORK_WOULD_BLOCK);
  CHECK(count == 0U && data[0] == 0xCCU && wio_network_get_state() == WIO_NETWORK_READY);
  CHECK(wio_network_get_diagnostics().remote_errno == 11);
  CHECK(wio_network_socket_close(socket, 100U) == WIO_NETWORK_OK);
  CHECK(wio_network_socket_close(socket, 100U) == WIO_NETWORK_STATE);
  wio_network_socket_t next = open_socket(WIO_NETWORK_TCP);
  CHECK(next != socket);
  CHECK(wio_network_socket_connect(next, &endpoint, 100U) == WIO_NETWORK_OK);
  peer.recv_result = 33;
  CHECK(wio_network_socket_receive(next, data, sizeof(data), &count, 100U) == WIO_NETWORK_PROTOCOL);
  CHECK(wio_network_get_state() == WIO_NETWORK_FAULTED && count == 0U);
  reset_peer(); attach();
  socket = open_socket(WIO_NETWORK_TCP);
  CHECK(wio_network_socket_connect(socket, &endpoint, 100U) == WIO_NETWORK_OK);
  peer.recv_result = -2;
  CHECK(wio_network_socket_receive(socket, data, sizeof(data), &count, 100U) == WIO_NETWORK_PROTOCOL);
}

static void test_udp_slots_and_poll(void) {
  reset_peer(); attach();
  wio_network_socket_t socket = open_socket(WIO_NETWORK_UDP);
  CHECK(wio_network_socket_bind(socket, &endpoint, 100U) == WIO_NETWORK_OK);
  size_t count;
  CHECK(wio_network_socket_sendto(socket, "abc", 3U, &endpoint, &count, 100U) == WIO_NETWORK_OK && count == 3U);
  uint8_t data[16]; memset(data, 0xCC, sizeof(data));
  wio_network_endpoint_t from;
  CHECK(wio_network_socket_recvfrom(socket, data, sizeof(data), &from, &count, 100U) == WIO_NETWORK_OK);
  CHECK(count == 3U && from.port == 0x1234U && from.address.octet[0] == 198U);
  CHECK(wio_network_socket_sendto(socket, NULL, 0U, &endpoint, &count, 100U) == WIO_NETWORK_ARGUMENT);
  peer.recv_result = 0; data[0] = 0xCCU;
  CHECK(wio_network_socket_recvfrom(socket, data, sizeof(data), &from, &count, 100U) == WIO_NETWORK_OK);
  CHECK(count == 0U && data[0] == 0xCCU && from.port == 0x1234U);
  wio_network_socket_t sockets[4] = {socket, 0U, 0U, 0U};
  for (unsigned i = 1U; i < 4U; ++i) { sockets[i] = open_socket(WIO_NETWORK_UDP); }
  uint32_t before = peer.sent;
  CHECK(wio_network_socket_open(WIO_NETWORK_UDP, &socket, 100U) == WIO_NETWORK_NO_SOCKET);
  CHECK(socket == 0U && peer.sent == before);
  for (unsigned i = 0U; i < 4U; ++i) { CHECK(wio_network_socket_close(sockets[i], 100U) == WIO_NETWORK_OK); }
  for (unsigned i = 0U; i < 10U; ++i) { event(7U); }
  CHECK(wio_network_poll(0U) == WIO_NETWORK_OK && peer.callback_count == 8U);
  CHECK(wio_network_poll(0U) == WIO_NETWORK_OK && peer.callback_count == 10U);
  const uint8_t partial[] = {60U, 0U}; queue(partial, sizeof(partial));
  CHECK(wio_network_poll(0U) == WIO_NETWORK_OK);
  peer.clock += 501U;
  CHECK(wio_network_poll(0U) == WIO_NETWORK_TIMEOUT && !peer.open);
}

static void test_timeouts_and_resource_faults(void) {
  reset_peer(); attach();
  peer.clock = UINT32_MAX - 5U;
  wio_network_socket_t socket = open_socket(WIO_NETWORK_TCP);
  peer.fault = NO_REPLY; peer.fault_service = 16U; peer.fault_method = 9U;
  CHECK(wio_network_socket_connect(socket, &endpoint, 20U) == WIO_NETWORK_TIMEOUT);
  CHECK(wio_network_get_state() == WIO_NETWORK_FAULTED);
  CHECK(wio_network_socket_close(socket, 20U) == WIO_NETWORK_STATE);
  reset_peer(); attach();
  socket = open_socket(WIO_NETWORK_UDP);
  peer.forced_fd = true; peer.force_fd = 0;
  CHECK(wio_network_socket_open(WIO_NETWORK_TCP, &socket, 100U) == WIO_NETWORK_PROTOCOL);
  CHECK(socket == 0U && wio_network_get_state() == WIO_NETWORK_FAULTED);
}

int main(void) {
  test_probe_and_arguments();
  test_faults();
  test_join_status_dns();
  test_tcp();
  test_udp_slots_and_poll();
  test_timeouts_and_resource_faults();
  reset_peer();
  printf("network protocol: %u assertions passed\n", assertions);
  return 0;
}
