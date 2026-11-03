#pragma once
#include "command_protocol.h"
#include <freertos/queue.h>
void sessionManagerBegin();
bool sessionManagerReady();
bool sessionManagerEnqueue(const ControllerCommand& command);
QueueHandle_t sessionCommandQueue();
QueueHandle_t sessionResultQueue();
void sessionManagerPoll();
