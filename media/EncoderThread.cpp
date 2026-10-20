#include "media/EncoderThread.h"

#include <thread>

#if defined(_WIN32)
#include <objbase.h>
#endif

namespace ve {

void runOnEncoderThread(const std::function<void()>& fn)
{
    std::thread worker([&] {
#if defined(_WIN32)
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        fn();
        if (SUCCEEDED(hr))
            CoUninitialize();
#else
        fn();
#endif
    });
    worker.join();
}

} // namespace ve
