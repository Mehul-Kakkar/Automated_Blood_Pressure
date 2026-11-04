#pragma once

// Phone BLE GATT protocol, version 1. Keep in sync with frontend/src/ble.ts.
namespace PhoneBle {
constexpr char DEVICE_NAME[] = "BP Controller";
constexpr char SERVICE_UUID[] = "4b505001-6b70-4c46-9c87-687572617465";
constexpr char DEVICE_INFO_UUID[] = "4b505002-6b70-4c46-9c87-687572617465";
constexpr char STATE_UUID[] = "4b505003-6b70-4c46-9c87-687572617465";
constexpr char PRESSURE_UUID[] = "4b505004-6b70-4c46-9c87-687572617465";
constexpr char HEART_RATE_UUID[] = "4b505005-6b70-4c46-9c87-687572617465";
constexpr char KOROTKOFF_UUID[] = "4b505006-6b70-4c46-9c87-687572617465";
constexpr char PROGRESS_UUID[] = "4b505007-6b70-4c46-9c87-687572617465";
constexpr char COMMAND_UUID[] = "4b505008-6b70-4c46-9c87-687572617465";
constexpr char RESPONSE_UUID[] = "4b505009-6b70-4c46-9c87-687572617465";
constexpr char FAULT_UUID[] = "4b50500a-6b70-4c46-9c87-687572617465";
constexpr uint8_t VERSION = 1;
}
