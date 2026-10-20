#pragma once

#include "core/Time.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace ve {

// Decoded picture handed from the engine to the viewer/exporter. Always planar 4:2:0 so the
// GPU side has two code paths only: 8-bit planes (R8 textures) or 16-bit planes (R16) for
// 10/12-bit sources. YUV -> RGB happens on the GPU, in float.
enum class PixelLayout { YUV420P8, YUV420P16 };
enum class ColorMatrix { BT601, BT709, BT2020 };

struct VideoFrame {
    int width = 0;
    int height = 0;
    PixelLayout layout = PixelLayout::YUV420P8;
    ColorMatrix matrix = ColorMatrix::BT709;
    bool fullRange = false;
    Time sourceTime; // presentation time in the media, relative to its start

    std::array<int, 3> planeWidth{};
    std::array<int, 3> planeHeight{};
    std::array<int, 3> stride{}; // bytes per row, 64-byte aligned

    // (Re)allocates the three planes for the given size and layout.
    void allocate(int w, int h, PixelLayout l);

    std::uint8_t* plane(int i) { return m_storage.data() + m_base + m_offset[std::size_t(i)]; }
    const std::uint8_t* plane(int i) const { return m_storage.data() + m_base + m_offset[std::size_t(i)]; }
    std::size_t planeBytes(int i) const { return std::size_t(stride[std::size_t(i)]) * std::size_t(planeHeight[std::size_t(i)]); }
    int bytesPerSample() const { return layout == PixelLayout::YUV420P16 ? 2 : 1; }

private:
    std::vector<std::uint8_t> m_storage;
    std::size_t m_base = 0; // aligns plane 0 to 64 bytes
    std::array<std::size_t, 3> m_offset{};
};

using VideoFramePtr = std::shared_ptr<const VideoFrame>;

} // namespace ve
