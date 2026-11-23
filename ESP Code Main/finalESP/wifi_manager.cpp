#include "wifi_manager.h"

#include <Arduino.h>
#include <Preferences.h>
#include <cstring>

#if __has_include("secrets.h")
#include "secrets.h"
#endif

namespace {
constexpr size_t WIFI_SSID_CAPACITY = 33;
constexpr size_t WIFI_PASSWORD_CAPACITY = 64;
constexpr char WIFI_PREFERENCES_NAMESPACE[] = "bp-wifi";

portMUX_TYPE wifiConfigMux = portMUX_INITIALIZER_UNLOCKED;
char activeSsid[WIFI_SSID_CAPACITY]{};
char activePassword[WIFI_PASSWORD_CAPACITY]{};
uint32_t activeGeneration = 0;
}

bool wifiManagerBegin() {
  char ssid[WIFI_SSID_CAPACITY]{};
  char password[WIFI_PASSWORD_CAPACITY]{};
  bool loadedFromNvs = false;

  Preferences preferences;
  if (preferences.begin(WIFI_PREFERENCES_NAMESPACE, true)) {
    const size_t ssidLength = preferences.getString("ssid", ssid, sizeof(ssid));
    preferences.getString("password", password, sizeof(password));
    loadedFromNvs = ssidLength > 0;
    preferences.end();
  }

  if (!ssid[0]) {
#if __has_include("secrets.h")
    strlcpy(ssid, BP_WIFI_SSID, sizeof(ssid));
    strlcpy(password, BP_WIFI_PASSWORD, sizeof(password));
#endif
  }

  portENTER_CRITICAL(&wifiConfigMux);
  strlcpy(activeSsid, ssid, sizeof(activeSsid));
  strlcpy(activePassword, password, sizeof(activePassword));
  activeGeneration = 1;
  portEXIT_CRITICAL(&wifiConfigMux);
  Serial.printf("Wi-Fi credentials at boot: %s; SSID configured=%s.\n",loadedFromNvs?"loaded from NVS":"using firmware defaults or none",ssid[0]?"yes":"no");
  return activeSsid[0] != '\0';
}

bool wifiManagerGetCredentials(char* ssid, size_t ssidCapacity, char* password, size_t passwordCapacity) {
  if (!ssid || !password || ssidCapacity == 0 || passwordCapacity == 0) return false;
  portENTER_CRITICAL(&wifiConfigMux);
  const size_t ssidLength = strlcpy(ssid, activeSsid, ssidCapacity);
  const size_t passwordLength = strlcpy(password, activePassword, passwordCapacity);
  const bool configured = activeSsid[0] != '\0' && ssidLength < ssidCapacity && passwordLength < passwordCapacity;
  portEXIT_CRITICAL(&wifiConfigMux);
  return configured;
}

bool wifiManagerSetCredentials(const char* ssid, const char* password) {
  if (!ssid || !password) return false;
  const size_t ssidLength = strnlen(ssid, WIFI_SSID_CAPACITY);
  const size_t passwordLength = strnlen(password, WIFI_PASSWORD_CAPACITY);
  if (ssidLength == 0 || ssidLength > 32 || passwordLength > 63 || (passwordLength > 0 && passwordLength < 8)) return false;

  Preferences preferences;
  if (!preferences.begin(WIFI_PREFERENCES_NAMESPACE, false)) return false;
  const size_t savedSsid = preferences.putString("ssid", ssid);
  const size_t savedPassword = preferences.putString("password", password);
  preferences.end();
  if (savedSsid == 0 || (passwordLength > 0 && savedPassword != passwordLength)) return false;

  portENTER_CRITICAL(&wifiConfigMux);
  strlcpy(activeSsid, ssid, sizeof(activeSsid));
  strlcpy(activePassword, password, sizeof(activePassword));
  ++activeGeneration;
  if (activeGeneration == 0) activeGeneration = 1;
  portEXIT_CRITICAL(&wifiConfigMux);
  Serial.println("Wi-Fi credentials committed to ESP32 NVS.");
  return true;
}

uint32_t wifiManagerGeneration() {
  portENTER_CRITICAL(&wifiConfigMux);
  const uint32_t generation = activeGeneration;
  portEXIT_CRITICAL(&wifiConfigMux);
  return generation;
}
