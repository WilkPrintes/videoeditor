#include "media/TestPattern.h"

#include "media/EncoderThread.h"
#include "media/Mp4Writer.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

namespace ve {

namespace {

bool writeTestClipImpl(const std::string& path, Time duration, Rate rate, int width, int height, std::string* error)
{
    width &= ~1;
    height &= ~1;
    const std::int64_t total = std::max<std::int64_t>(1, frameAt(duration, rate));
    const std::int64_t fps = std::max<std::int64_t>(1, std::llround(rate.toDouble()));

    Mp4Writer writer;
    if (!writer.open(path, rate, width, height, "auto", 8000, true, 128, error)) {
        removeFileQuietly(path);
        return false;
    }

    // 75% colour bars, BT.709 limited range: white, yellow, cyan, green, magenta, red, blue, black.
    static constexpr std::uint8_t barY[8] = {180, 168, 145, 134, 63, 51, 28, 16};
    static constexpr std::uint8_t barU[8] = {128, 44, 147, 63, 193, 109, 212, 128};
    static constexpr std::uint8_t barV[8] = {128, 136, 44, 52, 204, 212, 120, 128};

    std::vector<float> audio;
    std::int64_t samplesWritten = 0;
    for (std::int64_t f = 0; f < total; ++f) {
        AVFrame* frame = writer.beginVideoFrame();
        if (!frame) {
            *error = "Out of memory";
            removeFileQuietly(path);
            return false;
        }
        const bool flash = f % fps == 0;
        const int markerX = int((f * 8) % std::max(1, width - 16));
        const bool nv12 = frame->format == AV_PIX_FMT_NV12;
        for (int y = 0; y < height; ++y) {
            std::uint8_t* row = frame->data[0] + std::ptrdiff_t(y) * frame->linesize[0];
            for (int x = 0; x < width; ++x) {
                const bool marker = x >= markerX && x < markerX + 16 && y > height * 3 / 4;
                row[x] = flash || marker ? 235 : barY[x * 8 / width];
            }
        }
        for (int y = 0; y < height / 2; ++y) {
            for (int x = 0; x < width / 2; ++x) {
                const int bar = (x * 2) * 8 / width;
                const std::uint8_t u = flash ? 128 : barU[bar];
                const std::uint8_t v = flash ? 128 : barV[bar];
                if (nv12) {
                    std::uint8_t* row = frame->data[1] + std::ptrdiff_t(y) * frame->linesize[1];
                    row[x * 2] = u;
                    row[x * 2 + 1] = v;
                } else {
                    frame->data[1][std::ptrdiff_t(y) * frame->linesize[1] + x] = u;
                    frame->data[2][std::ptrdiff_t(y) * frame->linesize[2] + x] = v;
                }
            }
        }
        int r = writer.writeVideoFrame(f);
        if (r >= 0) {
            // 1 kHz beep during the flash frame, silence otherwise (sample-exact on the frame grid).
            const std::int64_t target = sampleAt(timeOfFrame(f + 1, rate), kEncodeAudioRate);
            const int n = int(target - samplesWritten);
            audio.assign(std::size_t(std::max(n, 0)) * 2, 0.0f);
            if (flash) {
                for (int i = 0; i < n; ++i) {
                    const double t = double(samplesWritten + i) / kEncodeAudioRate;
                    const float s = float(0.5 * std::sin(2.0 * std::numbers::pi * 1000.0 * t));
                    audio[std::size_t(i) * 2] = s;
                    audio[std::size_t(i) * 2 + 1] = s;
                }
            }
            samplesWritten = target;
            r = writer.writeAudio(audio.data(), n);
        }
        if (r < 0) {
            *error = "Encoding failed: " + ff::errorString(r);
            removeFileQuietly(path);
            return false;
        }
    }
    if (!writer.finish(error)) {
        removeFileQuietly(path);
        return false;
    }
    return true;
}

} // namespace

bool writeTestClip(const std::string& path, Time duration, Rate rate, int width, int height, std::string* error)
{
    std::string ignored;
    if (!error)
        error = &ignored;
    return runInEncoderThread([&] { return writeTestClipImpl(path, duration, rate, width, height, error); });
}

} // namespace ve
