#pragma once
#include <Arduino.h>
enum class CommandKind:uint8_t { START,STOP,STATUS_REQUEST,RESET_FAULT };
enum class CommandOrigin:uint8_t { MQTT,ESP_NOW_DISPLAY,BLE_PHONE };
struct ControllerCommand {
  CommandKind kind;
  CommandOrigin origin;
  uint16_t displaySequence;
  char commandId[37];
  char sessionId[37];
  char requestId[37];
};
struct CommandResult {
  CommandOrigin origin;
  uint16_t displaySequence;
  char commandId[37];
  char status[12];
  char reasonCode[65];
  char deviceState[12];
};
