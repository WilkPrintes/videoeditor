#include "core/Log.h"

#include <cstdio>
#include <mutex>

namespace ve {

namespace {

std::mutex& sinkMutex()
{
    static std::mutex m;
    return m;
}

LogSink& sink()
{
    static LogSink s;
    return s;
}

} // namespace

void setLogSink(LogSink s)
{
    std::lock_guard lock(sinkMutex());
    sink() = std::move(s);
}

void logMessage(LogLevel level, const std::string& message)
{
    std::lock_guard lock(sinkMutex());
    if (sink()) {
        sink()(level, message);
        return;
    }
    static constexpr const char* names[] = {"debug", "info", "warning", "error"};
    std::fprintf(stderr, "[%s] %s\n", names[int(level)], message.c_str());
}

} // namespace ve
