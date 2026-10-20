#pragma once

#include <functional>

namespace ve {

// Runs `fn` to completion on a fresh thread. On Windows that thread joins a multithreaded COM
// apartment: Media Foundation encoders require it, and FFmpeg's h264_mf calls CoUninitialize when
// it fails to open, which must never happen on the caller's (possibly STA/UI) thread.
void runOnEncoderThread(const std::function<void()>& fn);

template <typename F>
auto runInEncoderThread(F&& fn) -> decltype(fn())
{
    decltype(fn()) result{};
    runOnEncoderThread([&] { result = fn(); });
    return result;
}

} // namespace ve
