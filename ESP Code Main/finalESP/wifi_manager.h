#pragma once

#include <stddef.h>
#include <stdint.h>

bool wifiManagerBegin();
bool wifiManagerGetCredentials(char* ssid, size_t ssidCapacity, char* password, size_t passwordCapacity);
bool wifiManagerSetCredentials(const char* ssid, const char* password);
uint32_t wifiManagerGeneration();
