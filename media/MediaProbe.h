#pragma once

#include "core/Model.h"

#include <optional>
#include <string>

namespace ve {

// Reads container/stream metadata. The returned MediaItem has a fresh id.
std::optional<MediaItem> probeMedia(const std::string& path, std::string* error);

} // namespace ve
