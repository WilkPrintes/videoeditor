#pragma once

#include <functional>
#include <sstream>
#include <string>

namespace ve {

enum class LogLevel { Debug, Info, Warning, Error };

// The app routes this into Qt's message handler; the default sink writes to stderr.
using LogSink = std::function<void(LogLevel, const std::string&)>;
void setLogSink(LogSink sink);
void logMessage(LogLevel level, const std::string& message);

template <typename... Args>
void log(LogLevel level, const Args&... args)
{
    std::ostringstream s;
    (s << ... << args);
    logMessage(level, s.str());
}

template <typename... Args> void logInfo(const Args&... args) { log(LogLevel::Info, args...); }
template <typename... Args> void logWarning(const Args&... args) { log(LogLevel::Warning, args...); }
template <typename... Args> void logError(const Args&... args) { log(LogLevel::Error, args...); }

} // namespace ve
