
#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// Must match the display test firmware
static const uint8_t CHANNEL = 1;
static const uint32_t MAGIC = 0x42505031;
static const uint16_t VERSION = 1;

// Broadcast lets the display receive packets before peer discovery.
// The display test learns the main MAC from a valid received packet.
static uint8_t DISPLAY_BROADCAST_MAC[6] = {
  0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

struct __attribute__((packed)) Packet {
  uint32_t magic;
  uint16_t version;
  uint16_t seq;
  char type[16];
  char payload[180];
};

static uint16_t seqNo = 0;
static uint32_t lastSend = 0;

String macString(const uint8_t *mac) {
  char buf[18];
  snprintf(buf, sizeof(buf),
           "%02X:%02X:%02X:%02X:%02X:%02X",
           mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5]);
  return String(buf);
}

void onReceive(const esp_now_recv_info_t *info,
               const uint8_t *data, int len) {
  if (len != (int)sizeof(Packet)) {
    Serial.printf("[RX] Ignored packet: length=%d\n", len);
    return;
  }

  Packet p{};
  memcpy(&p, data, sizeof(p));

  if (p.magic != MAGIC || p.version != VERSION) {
    Serial.println("[RX] Ignored packet: invalid header");
    return;
  }

  Serial.printf("[RX] From %s | type=%s | seq=%u\n",
                macString(info->src_addr).c_str(),
                p.type, p.seq);
  Serial.printf("Payload: %s\n", p.payload);
}

void onSent(const wifi_tx_info_t *info,
            esp_now_send_status_t status) {
  Serial.printf("[TX status] %s\n",
      status == ESP_NOW_SEND_SUCCESS ? "success" : "fail");
}

void sendPacket(const char *type, const char *payload) {
  Packet p{};
  p.magic = MAGIC;
  p.version = VERSION;
  p.seq = ++seqNo;

  strlcpy(p.type, type, sizeof(p.type));
  strlcpy(p.payload, payload, sizeof(p.payload));

  esp_err_t result = esp_now_send(
      DISPLAY_BROADCAST_MAC,
      reinterpret_cast<const uint8_t *>(&p),
      sizeof(p)
  );

  Serial.printf("[TX %u] result=%d type=%s payload=%s\n",
                p.seq, (int)result, p.type, p.payload);
}

void setup() {
  Serial.begin(115200);
  delay(1500);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);
  delay(100);

  Serial.println();
  Serial.println("Main ESP32-S3 ESP-NOW binary protocol test");
  Serial.printf("Main STA MAC: %s\n", WiFi.macAddress().c_str());

  esp_err_t channelResult =
      esp_wifi_set_channel(CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.printf("Set channel %u: %d\n",
                CHANNEL, (int)channelResult);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ERROR: ESP-NOW initialization failed");
    return;
  }

  esp_now_register_recv_cb(onReceive);
  esp_now_register_send_cb(onSent);

  // Register broadcast peer for ESP-NOW transmission.
  esp_now_peer_info_t peer{};
  memset(peer.peer_addr, 0xFF, 6);
  peer.channel = CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;

  esp_err_t peerResult = esp_now_add_peer(&peer);
  Serial.printf("Add broadcast peer: %d\n", (int)peerResult);

  Serial.println("Sending compatible test packets...");
  sendPacket("MAIN_TEST",
             "{\"source\":\"main\",\"test\":true}");
}

void loop() {
  if (millis() - lastSend >= 2000) {
    lastSend = millis();

    char payload[180];
    snprintf(payload, sizeof(payload),
             "{\"source\":\"main\",\"uptime_ms\":%lu}",
             (unsigned long)millis());

    sendPacket("MAIN_TEST", payload);
  }

  delay(10);
}
