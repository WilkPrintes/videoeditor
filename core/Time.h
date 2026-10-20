#pragma once

// Rational time (spec §5.1). Every position and duration is an integer count of flicks,
// 1/705,600,000 s. A flick divides the frame duration of every common cadence (23.976 = 24000/1001,
// 24, 25, 29.97, 30, 48, 50, 59.94, 60, 120 fps) and the sample duration of 44.1/48/88.2/96/192 kHz
// exactly, so edits never drift. Never use double for timeline positions: frames and seconds exist
// only at the edges (UI, decoding), through frameAt()/timeOfFrame() and friends.

#include <compare>
#include <cstdint>
#include <limits>
#include <string>

namespace ve {

inline constexpr std::int64_t kFlicksPerSecond = 705'600'000;

struct Time {
    std::int64_t flicks = 0;

    constexpr Time() = default;
    constexpr explicit Time(std::int64_t f) : flicks(f) {}

    static constexpr Time fromSeconds(std::int64_t s) { return Time(s * kFlicksPerSecond); }
    static constexpr Time max() { return Time(std::numeric_limits<std::int64_t>::max() / 4); }

    // For display and for APIs that want seconds (never for editing arithmetic).
    double toSeconds() const { return double(flicks) / double(kFlicksPerSecond); }

    constexpr auto operator<=>(const Time&) const = default;
    constexpr Time operator+(Time o) const { return Time(flicks + o.flicks); }
    constexpr Time operator-(Time o) const { return Time(flicks - o.flicks); }
    constexpr Time operator-() const { return Time(-flicks); }
    constexpr Time& operator+=(Time o) { flicks += o.flicks; return *this; }
    constexpr Time& operator-=(Time o) { flicks -= o.flicks; return *this; }
};

struct TimeRange {
    Time start;
    Time duration;

    constexpr Time end() const { return start + duration; }
    constexpr bool contains(Time t) const { return t >= start && t < end(); }
    bool operator==(const TimeRange&) const = default;
};

// A frame rate or any other rational rate (num/den per second).
struct Rate {
    int num = 30;
    int den = 1;

    double toDouble() const { return double(num) / double(den); }
    bool isValid() const { return num > 0 && den > 0; }
    bool operator==(const Rate&) const = default;
};

// floor(a * b / c) and ceil(a * b / c) for b >= 0, c > 0 without intermediate overflow in the
// ranges used here (hours of media at any common rate).
std::int64_t mulDivFloor(std::int64_t a, std::int64_t b, std::int64_t c);
std::int64_t mulDivCeil(std::int64_t a, std::int64_t b, std::int64_t c);

// Index of the frame whose window [timeOfFrame(n), timeOfFrame(n + 1)) contains t.
std::int64_t frameAt(Time t, Rate rate);
// Start of frame n; rounded up to the next flick for rates that do not divide a second, which keeps
// frameAt(timeOfFrame(n)) == n for every n.
Time timeOfFrame(std::int64_t n, Rate rate);
// t moved down to the start of its frame.
Time floorToFrame(Time t, Rate rate);

// The same for audio samples.
std::int64_t sampleAt(Time t, int sampleRate);
Time timeOfSample(std::int64_t s, int sampleRate);

// Conversions at the media edge (container durations come in seconds or microseconds).
Time timeFromSeconds(double seconds);
Time timeFromMicroseconds(std::int64_t us);

// HH:MM:SS:FF, non-drop-frame, using the nominal (rounded) rate.
std::string formatTimecode(Time t, Rate rate);
// "HH:MM:SS:FF" or "HH:MM:SS;FF" to a time at `rate` (nominal counting). False if malformed.
bool parseTimecode(const std::string& text, Rate rate, Time* out);

// Snaps measured rates to the usual broadcast fractions (29.97 -> 30000/1001).
Rate rateFromDouble(double fps);

} // namespace ve
