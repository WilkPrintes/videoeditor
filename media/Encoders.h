#pragma once

// FFmpeg-free view of the encoders, for the UI and tools.

#include <string>
#include <vector>

namespace ve {

// H.264 encoders compiled into this FFmpeg, in "auto" preference order. `usableOnly` also opens
// each one briefly to check the hardware is present.
std::vector<std::string> availableH264Encoders(bool usableOnly);

} // namespace ve
