#pragma once

#include "core/Snapshot.h"
#include "media/Encoders.h"

#include <cstdint>
#include <functional>
#include <string>

namespace ve {

struct ExportSettings {
    std::string outputPath;            // .mp4
    int width = 0;                     // 0 = timeline size
    int height = 0;
    int videoBitrateKbps = 12000;
    int audioBitrateKbps = 192;
    std::string videoEncoder = "auto"; // or an FFmpeg encoder name, e.g. "h264_nvenc"
    // Fingerprint every rendered picture and the mixed sound before encoding (ExportResult hashes),
    // so two renders can be compared exactly whatever the encoder does.
    bool renderHash = false;
};

struct ExportResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;
    std::string videoEncoder; // the encoder that was actually used
    std::int64_t framesWritten = 0;
    std::uint64_t videoHash = 0; // with ExportSettings::renderHash
    std::uint64_t audioHash = 0;
};

// Progress in [0, 1]; return false to cancel.
using ExportProgress = std::function<bool(double)>;

// Renders the timeline to H.264 + AAC in MP4, frame by frame on the timeline's frame grid, with the
// audio mixed by the same AudioMixer as playback. The LGPL FFmpeg build has no x264, so "auto"
// tries hardware encoders first (see media/Mp4Writer.h).
ExportResult exportTimeline(const TimelineSnapshot& snap, const ExportSettings& settings,
                            const ExportProgress& progress = {});

} // namespace ve
