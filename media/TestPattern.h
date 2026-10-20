#pragma once

#include "core/Time.h"

#include <string>

namespace ve {

// Writes a synthetic H.264/AAC clip for tests: colour bars, a marker whose position follows the
// frame number, and a white flash plus a 1 kHz beep on the first frame of every second (the
// "clapper" of spec phase 1: A/V sync can be checked by eye, by ear and by measurement).
bool writeTestClip(const std::string& path, Time duration, Rate rate, int width, int height, std::string* error);

} // namespace ve
