// The SuperMini's blue LED (installer/SPEC.md, "LED"): 1 Hz idle, 5 Hz during a
// WiFi install, a header install or an installer update, on for 10 s after an
// install completed, 3 short blinks every 2 s while there is no complete carried
// image.

#pragma once

void led_start(void);
// A WiFi install sent every byte, or a header install verified every region.
void led_completed(void);
