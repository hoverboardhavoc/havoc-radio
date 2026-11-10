// The lamp on the back (GPIO15). Off at power-up. The output is driven from a FreeRTOS timer, so SOS keeps its
// timing whatever the UI loop is doing.
#pragma once

#include "radio_model.h"

namespace lamp {

void start(int level);
void setMode(radio::Lamp m);  // SOS starts with its first dot when chosen
void setLevel(int level);
radio::Lamp mode();

}  // namespace lamp
