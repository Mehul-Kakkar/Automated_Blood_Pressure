#pragma once
#include "command_protocol.h"
#include <freertos/queue.h>
void displayEspNowBegin();
void displayPublishStatus(const char* statusJson);
void displayPublishResult(const CommandResult& result);
