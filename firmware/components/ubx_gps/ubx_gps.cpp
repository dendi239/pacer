#include "ubx_gps.hpp"

#include <cstring>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace {

const char *TAG = "ubx_gps";

constexpr uart_port_t kUart = (uart_port_t)CONFIG_PACER_GPS_UART_NUM;
constexpr int kBaud = CONFIG_PACER_GPS_BAUD;

constexpr uint8_t kSync1 = 0xB5, kSync2 = 0x62;
constexpr uint8_t kClassNav = 0x01, kIdNavPvt = 0x07;
constexpr uint8_t kClassCfg = 0x06, kIdCfgValset = 0x8A;
constexpr size_t kNavPvtLen = 92;

ubx_pvt_callback_t s_callback = nullptr;
void *s_ctx = nullptr;

//------------------------------ wire helpers ------------------------------//

uint16_t get_u2(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t get_u4(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
int32_t get_i4(const uint8_t *p) { return (int32_t)get_u4(p); }
int16_t get_i2(const uint8_t *p) { return (int16_t)get_u2(p); }

// Offsets per the u-blox interface manual (UBX-NAV-PVT, 92-byte payload).
// The wire fixType is one byte; the shared struct keeps it as an enum (an
// int), which is why this is a field-by-field decode and not a memcpy.
void decode_nav_pvt(const uint8_t *p, uGnssDecUbxNavPvt_t &out) {
  out.iTOW = get_u4(p + 0);
  out.year = get_u2(p + 4);
  out.month = p[6];
  out.day = p[7];
  out.hour = p[8];
  out.min = p[9];
  out.sec = p[10];
  out.valid = p[11];
  out.tAcc = get_u4(p + 12);
  out.nano = get_i4(p + 16);
  out.fixType = (uGnssDecUbxNavPvtFixType_t)p[20];
  out.flags = p[21];
  out.flags2 = p[22];
  out.numSV = p[23];
  out.lon = get_i4(p + 24);
  out.lat = get_i4(p + 28);
  out.height = get_i4(p + 32);
  out.hMSL = get_i4(p + 36);
  out.hAcc = get_u4(p + 40);
  out.vAcc = get_u4(p + 44);
  out.velN = get_i4(p + 48);
  out.velE = get_i4(p + 52);
  out.velD = get_i4(p + 56);
  out.gSpeed = get_i4(p + 60);
  out.headMot = get_i4(p + 64);
  out.sAcc = get_u4(p + 68);
  out.headAcc = get_u4(p + 72);
  out.pDOP = get_u2(p + 76);
  out.flags3 = get_u2(p + 78);
  // p+80..83 reserved
  out.headVeh = get_i4(p + 84);
  out.magDec = get_i2(p + 88);
  out.magAcc = get_u2(p + 90);
}

//----------------------------- frame sending ------------------------------//

void ubx_checksum(const uint8_t *data, size_t len, uint8_t &ck_a,
                  uint8_t &ck_b) {
  ck_a = 0;
  ck_b = 0;
  for (size_t i = 0; i < len; ++i) {
    ck_a += data[i];
    ck_b += ck_a;
  }
}

void send_ubx(uint8_t cls, uint8_t id, const uint8_t *payload, uint16_t len) {
  uint8_t frame[8 + 512];
  if (len > 512) {
    return;
  }
  frame[0] = kSync1;
  frame[1] = kSync2;
  frame[2] = cls;
  frame[3] = id;
  frame[4] = (uint8_t)(len & 0xFF);
  frame[5] = (uint8_t)(len >> 8);
  memcpy(frame + 6, payload, len);
  uint8_t ck_a, ck_b;
  ubx_checksum(frame + 2, 4 + len, ck_a, ck_b);
  frame[6 + len] = ck_a;
  frame[7 + len] = ck_b;
  uart_write_bytes(kUart, frame, 8 + len);
  uart_wait_tx_done(kUart, pdMS_TO_TICKS(200));
}

// CFG-VALSET with a list of key/value items (values already little-endian,
// sized per key). Layer: RAM only — bit 2 (0x04) would be the flash layer,
// which the M10 lacks; including it makes the receiver NAK the whole set.
struct CfgItem {
  uint32_t key;
  uint32_t value;
  uint8_t value_size; // 1, 2 or 4
};

void send_valset(const CfgItem *items, size_t count) {
  uint8_t payload[256];
  size_t n = 0;
  payload[n++] = 0x00; // version
  payload[n++] = 0x01; // layers: RAM
  payload[n++] = 0x00; // reserved
  payload[n++] = 0x00;
  for (size_t i = 0; i < count; ++i) {
    const CfgItem &it = items[i];
    payload[n++] = (uint8_t)(it.key & 0xFF);
    payload[n++] = (uint8_t)((it.key >> 8) & 0xFF);
    payload[n++] = (uint8_t)((it.key >> 16) & 0xFF);
    payload[n++] = (uint8_t)((it.key >> 24) & 0xFF);
    for (int b = 0; b < it.value_size; ++b) {
      payload[n++] = (uint8_t)((it.value >> (8 * b)) & 0xFF);
    }
  }
  send_ubx(kClassCfg, kIdCfgValset, payload, (uint16_t)n);
}

// Configuration keys (u-blox M8 ignores VALSET — M9/M10 assumed, which is
// what runs 25 Hz anyway).
constexpr uint32_t kKeyRateMeas = 0x30210001;       // U2, ms per measurement
constexpr uint32_t kKeyMsgoutPvtUart1 = 0x20910007; // U1, rate on UART1
constexpr uint32_t kKeyUart1OutUbx = 0x10740001;    // bool
constexpr uint32_t kKeyUart1OutNmea = 0x10740002;   // bool
constexpr uint32_t kKeyUart1Baud = 0x40520001;      // U4

void push_config(bool include_baud) {
  CfgItem items[5];
  size_t n = 0;
  if (include_baud) {
    items[n++] = {kKeyUart1Baud, (uint32_t)kBaud, 4};
  }
  items[n++] = {kKeyUart1OutUbx, 1, 1};
  items[n++] = {kKeyUart1OutNmea, 0, 1};
  items[n++] = {kKeyRateMeas, 40, 2}; // 40 ms -> 25 Hz
  items[n++] = {kKeyMsgoutPvtUart1, 1, 1};
  send_valset(items, n);
}

//------------------------------ reader task -------------------------------//

void reader_task(void *) {
  // Frame parser state machine.
  enum class St { Sync1, Sync2, Class, Id, Len1, Len2, Payload, CkA, CkB };
  St st = St::Sync1;
  uint8_t cls = 0, id = 0, ck_a = 0, ck_b = 0;
  uint16_t len = 0, got = 0;
  static uint8_t payload[1024];

  uint8_t buf[256];
  TickType_t last_pvt = xTaskGetTickCount();
  size_t resync_attempt = 0;
  size_t bytes_since_resync = 0;

  while (true) {
    int n = uart_read_bytes(kUart, buf, sizeof(buf), pdMS_TO_TICKS(100));
    if (n > 0) {
      bytes_since_resync += n;
    }

    // No valid PVT for a while: the receiver is at a different baud (9600
    // factory default, or whatever a previous firmware left it at — a wrong
    // baud still yields garbage bytes, so don't gate this on silence).
    // Push the config (incl. baud switch) through each candidate in turn,
    // then go back to the configured baud and re-push.
    if (xTaskGetTickCount() - last_pvt > pdMS_TO_TICKS(3000)) {
      static constexpr int kCandidateBauds[] = {9600,   921600, 460800,
                                                230400, 38400,  kBaud};
      int candidate =
          kCandidateBauds[resync_attempt++ % (sizeof(kCandidateBauds) /
                                              sizeof(kCandidateBauds[0]))];
      // 0 bytes = dead line (wiring/power); >0 = alive but wrong baud or
      // protocol.
      ESP_LOGW(TAG,
               "no NAV-PVT at %d baud (%u bytes seen), configuring receiver "
               "via %d",
               kBaud, (unsigned)bytes_since_resync, candidate);
      bytes_since_resync = 0;
      uart_set_baudrate(kUart, candidate);
      push_config(/*include_baud=*/true);
      vTaskDelay(pdMS_TO_TICKS(200));
      uart_set_baudrate(kUart, kBaud);
      push_config(/*include_baud=*/false);
      uart_flush_input(kUart);
      st = St::Sync1;
      last_pvt = xTaskGetTickCount();
      continue;
    }

    for (int i = 0; i < n; ++i) {
      uint8_t b = buf[i];
      switch (st) {
      case St::Sync1:
        st = (b == kSync1) ? St::Sync2 : St::Sync1;
        break;
      case St::Sync2:
        st = (b == kSync2) ? St::Class : St::Sync1;
        break;
      case St::Class:
        cls = b;
        ck_a = b;
        ck_b = ck_a;
        st = St::Id;
        break;
      case St::Id:
        id = b;
        ck_a += b;
        ck_b += ck_a;
        st = St::Len1;
        break;
      case St::Len1:
        len = b;
        ck_a += b;
        ck_b += ck_a;
        st = St::Len2;
        break;
      case St::Len2:
        len |= (uint16_t)b << 8;
        ck_a += b;
        ck_b += ck_a;
        got = 0;
        if (len > sizeof(payload)) {
          st = St::Sync1;
        } else {
          st = len ? St::Payload : St::CkA;
        }
        break;
      case St::Payload:
        payload[got++] = b;
        ck_a += b;
        ck_b += ck_a;
        if (got == len) {
          st = St::CkA;
        }
        break;
      case St::CkA:
        st = (b == ck_a) ? St::CkB : St::Sync1;
        break;
      case St::CkB:
        if (b == ck_b && cls == kClassNav && id == kIdNavPvt &&
            len >= kNavPvtLen) {
          uGnssDecUbxNavPvt_t pvt;
          decode_nav_pvt(payload, pvt);
          last_pvt = xTaskGetTickCount();
          resync_attempt = 0;
          if (s_callback) {
            s_callback(pvt, s_ctx);
          }
        }
        st = St::Sync1;
        break;
      }
    }
  }
}

} // namespace

esp_err_t ubx_gps_start(ubx_pvt_callback_t on_pvt, void *ctx) {
  s_callback = on_pvt;
  s_ctx = ctx;

  uart_config_t cfg = {};
  cfg.baud_rate = kBaud;
  cfg.data_bits = UART_DATA_8_BITS;
  cfg.parity = UART_PARITY_DISABLE;
  cfg.stop_bits = UART_STOP_BITS_1;
  cfg.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  cfg.source_clk = UART_SCLK_DEFAULT;

  ESP_ERROR_CHECK(uart_driver_install(kUart, 4096, 0, 0, nullptr, 0));
  ESP_ERROR_CHECK(uart_param_config(kUart, &cfg));
  ESP_ERROR_CHECK(uart_set_pin(kUart, CONFIG_PACER_GPS_TX_GPIO,
                               CONFIG_PACER_GPS_RX_GPIO, UART_PIN_NO_CHANGE,
                               UART_PIN_NO_CHANGE));

  push_config(/*include_baud=*/true);

  if (xTaskCreate(reader_task, "ubx_gps", 6144, nullptr, 10, nullptr) !=
      pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "started (uart %d, rx %d, tx %d, %d baud)", (int)kUart,
           CONFIG_PACER_GPS_RX_GPIO, CONFIG_PACER_GPS_TX_GPIO, kBaud);
  return ESP_OK;
}
