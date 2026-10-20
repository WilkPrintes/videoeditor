#include "core/Time.h"

#include <catch_amalgamated.hpp>

using namespace ve;

namespace {
const Rate kCommonRates[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1}, {48, 1},
                             {50, 1}, {60000, 1001}, {60, 1}, {120000, 1001}, {120, 1}};
const int kSampleRates[] = {44100, 48000, 88200, 96000, 192000};
} // namespace

TEST_CASE("a flick divides every common frame and sample duration exactly", "[time]")
{
    for (const Rate& r : kCommonRates) {
        INFO(r.num << "/" << r.den);
        CHECK((kFlicksPerSecond * r.den) % r.num == 0);
    }
    for (int sr : kSampleRates) {
        INFO(sr);
        CHECK(kFlicksPerSecond % sr == 0);
    }
}

TEST_CASE("frame <-> time round trips and window rule", "[time]")
{
    for (const Rate& r : {kCommonRates[0], kCommonRates[3], kCommonRates[6], Rate{12345, 1000}, Rate{7, 3}}) {
        INFO(r.num << "/" << r.den);
        for (std::int64_t n : {std::int64_t(-5), std::int64_t(0), std::int64_t(1), std::int64_t(29),
                               std::int64_t(30), std::int64_t(1001), std::int64_t(86'400 * 120)}) {
            const Time t = timeOfFrame(n, r);
            CHECK(frameAt(t, r) == n);
            // The instant just before a frame starts belongs to the previous frame.
            CHECK(frameAt(t - Time(1), r) == n - 1);
            CHECK(floorToFrame(t + Time(1), r) == t);
        }
    }
    // Exact durations for the broadcast rates.
    CHECK(timeOfFrame(1, {30000, 1001}).flicks == 23'543'520);
    CHECK(timeOfFrame(1, {24, 1}).flicks == 29'400'000);
    CHECK(timeOfFrame(30000, {30000, 1001}) == Time::fromSeconds(1001));
}

TEST_CASE("sample <-> time round trips", "[time]")
{
    for (int sr : kSampleRates) {
        for (std::int64_t s : {std::int64_t(0), std::int64_t(1), std::int64_t(sr - 1), std::int64_t(sr) * 3600 * 24}) {
            CHECK(sampleAt(timeOfSample(s, sr), sr) == s);
        }
    }
    // One frame at 29.97 fps is 1601.6 samples at 48 kHz: the frame grid and the sample grid only
    // meet every 5 frames.
    CHECK(sampleAt(timeOfFrame(1, {30000, 1001}), 48000) == 1601);
    CHECK(sampleAt(timeOfFrame(5, {30000, 1001}), 48000) == 8008);
}

TEST_CASE("mulDiv floors and ceils without overflow", "[time]")
{
    CHECK(mulDivFloor(7, 3, 2) == 10);
    CHECK(mulDivCeil(7, 3, 2) == 11);
    CHECK(mulDivFloor(-7, 3, 2) == -11);
    CHECK(mulDivCeil(-7, 3, 2) == -10);
    // 24 h at 120000/1001: the naive product would overflow int64.
    const Time day = Time::fromSeconds(86'400);
    const std::int64_t frames = frameAt(day, {120000, 1001});
    CHECK(frames == 10'357'642); // 86400 * 120000 / 1001, floored
    CHECK(frameAt(timeOfFrame(frames, {120000, 1001}), {120000, 1001}) == frames);
}

TEST_CASE("timecode formatting and parsing", "[time]")
{
    CHECK(formatTimecode(Time(0), {30, 1}) == "00:00:00:00");
    CHECK(formatTimecode(timeOfFrame(30 * 61 + 5, {30, 1}), {30, 1}) == "00:01:01:05");
    CHECK(formatTimecode(timeOfFrame(24 * 3600, {24, 1}), {24, 1}) == "01:00:00:00");
    CHECK(formatTimecode(timeOfFrame(-1, {25, 1}), {25, 1}) == "-00:00:00:01");

    Time t;
    REQUIRE(parseTimecode("01:00:10:12", {25, 1}, &t));
    CHECK(frameAt(t, {25, 1}) == (3600 + 10) * 25 + 12);
    REQUIRE(parseTimecode("00:00:01;02", {30000, 1001}, &t));
    CHECK(frameAt(t, {30000, 1001}) == 32);
    CHECK_FALSE(parseTimecode("garbage", {25, 1}, &t));
    CHECK_FALSE(parseTimecode("00:61:00:00", {25, 1}, &t));
}

TEST_CASE("measured frame rates snap to broadcast fractions", "[time]")
{
    CHECK((rateFromDouble(29.97002997) == Rate{30000, 1001}));
    CHECK((rateFromDouble(23.976) == Rate{24000, 1001}));
    CHECK((rateFromDouble(25.0) == Rate{25, 1}));
    CHECK((rateFromDouble(0.0) == Rate{30, 1}));
    CHECK((rateFromDouble(14.985) == Rate{14985, 1000}));
}
