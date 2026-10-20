#include "core/Time.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace ve {

std::int64_t mulDivFloor(std::int64_t a, std::int64_t b, std::int64_t c)
{
    // a = q*c + r with 0 <= r < c, so a*b/c = q*b + r*b/c and r*b stays small.
    std::int64_t q = a / c;
    std::int64_t r = a % c;
    if (r < 0) {
        r += c;
        q -= 1;
    }
    return q * b + (r * b) / c;
}

std::int64_t mulDivCeil(std::int64_t a, std::int64_t b, std::int64_t c)
{
    return -mulDivFloor(-a, b, c);
}

std::int64_t frameAt(Time t, Rate rate)
{
    return mulDivFloor(t.flicks, rate.num, kFlicksPerSecond * rate.den);
}

Time timeOfFrame(std::int64_t n, Rate rate)
{
    return Time(mulDivCeil(n, kFlicksPerSecond * rate.den, rate.num));
}

Time floorToFrame(Time t, Rate rate)
{
    return timeOfFrame(frameAt(t, rate), rate);
}

std::int64_t sampleAt(Time t, int sampleRate)
{
    return mulDivFloor(t.flicks, sampleRate, kFlicksPerSecond);
}

Time timeOfSample(std::int64_t s, int sampleRate)
{
    return Time(mulDivCeil(s, kFlicksPerSecond, sampleRate));
}

Time timeFromSeconds(double seconds)
{
    return Time(std::llround(seconds * double(kFlicksPerSecond)));
}

Time timeFromMicroseconds(std::int64_t us)
{
    return Time(mulDivFloor(us, kFlicksPerSecond, 1'000'000));
}

std::string formatTimecode(Time t, Rate rate)
{
    std::int64_t frames = frameAt(t, rate);
    const char* sign = frames < 0 ? "-" : "";
    if (frames < 0)
        frames = -frames;
    const std::int64_t nominal = std::max<std::int64_t>(1, std::llround(rate.toDouble()));
    const std::int64_t ff = frames % nominal;
    const std::int64_t secs = frames / nominal;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%s%02lld:%02lld:%02lld:%02lld", sign, static_cast<long long>(secs / 3600),
                  static_cast<long long>((secs / 60) % 60), static_cast<long long>(secs % 60),
                  static_cast<long long>(ff));
    return buf;
}

bool parseTimecode(const std::string& text, Rate rate, Time* out)
{
    int h = 0, m = 0, s = 0, f = 0;
    char sep = 0;
    if (std::sscanf(text.c_str(), "%d:%d:%d%c%d", &h, &m, &s, &sep, &f) != 5 || (sep != ':' && sep != ';'))
        return false;
    if (h < 0 || m < 0 || m > 59 || s < 0 || s > 59 || f < 0)
        return false;
    const std::int64_t nominal = std::max<std::int64_t>(1, std::llround(rate.toDouble()));
    *out = timeOfFrame((std::int64_t(h) * 3600 + m * 60 + s) * nominal + f, rate);
    return true;
}

Rate rateFromDouble(double fps)
{
    static constexpr std::array<Rate, 12> known{{
        {24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {48, 1},
        {50, 1}, {60000, 1001}, {60, 1}, {100, 1}, {120000, 1001}, {120, 1},
    }};
    for (const Rate& r : known) {
        if (std::abs(r.toDouble() - fps) < 0.002)
            return r;
    }
    if (!(fps > 0.0) || !std::isfinite(fps))
        return {30, 1};
    return {int(std::llround(fps * 1000.0)), 1000};
}

} // namespace ve
