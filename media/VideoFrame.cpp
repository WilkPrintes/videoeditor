#include "media/VideoFrame.h"

namespace ve {

namespace {
constexpr std::size_t kAlign = 64;
std::size_t alignUp(std::size_t v) { return (v + kAlign - 1) & ~(kAlign - 1); }
} // namespace

void VideoFrame::allocate(int w, int h, PixelLayout l)
{
    width = w;
    height = h;
    layout = l;
    const int bps = bytesPerSample();
    const int cw = (w + 1) / 2;
    const int ch = (h + 1) / 2;
    planeWidth = {w, cw, cw};
    planeHeight = {h, ch, ch};

    std::size_t total = 0;
    for (std::size_t i = 0; i < 3; ++i) {
        stride[i] = int(alignUp(std::size_t(planeWidth[i]) * std::size_t(bps)));
        m_offset[i] = total;
        total += alignUp(std::size_t(stride[i]) * std::size_t(planeHeight[i]));
    }
    m_storage.resize(total + kAlign);
    const auto addr = reinterpret_cast<std::uintptr_t>(m_storage.data());
    m_base = std::size_t((kAlign - (addr % kAlign)) % kAlign);
}

} // namespace ve
