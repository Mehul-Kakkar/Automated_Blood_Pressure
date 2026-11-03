#pragma once
#include "command_protocol.h"
void bleManagerBegin();
void bleManagerPoll();
void bleManagerPublishCommandResult(const CommandResult& result);
