#include "media/FFmpegUtil.h"

#include "core/Log.h"

#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <mutex>

namespace ve::ff {

void installLogBridge()
{
    static std::once_flag once;
    std::call_once(once, [] {
        const char* env = std::getenv("VE_FFMPEG_LOG");
        av_log_set_level(env && *env ? AV_LOG_INFO : AV_LOG_ERROR);
        av_log_set_callback([](void* avcl, int level, const char* fmt, va_list vl) {
            if (level > av_log_get_level())
                return;
            char line[1024];
            int printPrefix = 1;
            av_log_format_line2(avcl, level, fmt, vl, line, sizeof line, &printPrefix);
            std::string msg(line);
            while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r'))
                msg.pop_back();
            if (msg.empty())
                return;
            const LogLevel l = level <= AV_LOG_ERROR ? LogLevel::Error
                : level <= AV_LOG_WARNING            ? LogLevel::Warning
                                                     : LogLevel::Info;
            log(l, "ffmpeg: ", msg);
        });
    });
}

std::string errorString(int err)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof buf);
    return buf;
}

double toSeconds(std::int64_t ts, AVRational tb)
{
    return double(ts) * av_q2d(tb);
}

std::int64_t fromSeconds(double seconds, AVRational tb)
{
    return std::int64_t(std::llround(seconds / av_q2d(tb)));
}

InputFormatPtr openInput(const std::string& path, std::string* error)
{
    installLogBridge();
    AVFormatContext* raw = nullptr;
    int r = avformat_open_input(&raw, path.c_str(), nullptr, nullptr);
    if (r < 0) {
        if (error)
            *error = "Cannot open " + path + ": " + errorString(r);
        return nullptr;
    }
    InputFormatPtr fmt(raw);
    r = avformat_find_stream_info(fmt.get(), nullptr);
    if (r < 0) {
        if (error)
            *error = "Cannot read stream info: " + errorString(r);
        return nullptr;
    }
    return fmt;
}

ve::Time containerStart(const AVFormatContext* fmt)
{
    if (fmt->start_time == AV_NOPTS_VALUE)
        return ve::Time(0);
    return ve::timeFromMicroseconds(fmt->start_time); // AV_TIME_BASE units
}

AVHWDeviceType platformHwDeviceType()
{
#if defined(_WIN32)
    return AV_HWDEVICE_TYPE_D3D11VA;
#elif defined(__APPLE__)
    return AV_HWDEVICE_TYPE_VIDEOTOOLBOX;
#else
    return AV_HWDEVICE_TYPE_VAAPI;
#endif
}

} // namespace ve::ff
