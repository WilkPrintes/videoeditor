#include "core/Id.h"

#include <cstdint>
#include <cstdio>
#include <random>

namespace ve {

namespace {

struct Sequence {
    bool active = false;
    std::string prefix;
    std::uint64_t next = 1;
};

thread_local Sequence t_sequence;

} // namespace

Id newId()
{
    if (t_sequence.active) {
        char digits[24];
        std::snprintf(digits, sizeof digits, "%04llu", static_cast<unsigned long long>(t_sequence.next++));
        return t_sequence.prefix + digits;
    }
    thread_local std::mt19937_64 rng{[] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(), rd(), rd(), rd(), rd()};
        return std::mt19937_64(seq);
    }()};
    static constexpr char hex[] = "0123456789abcdef";
    Id id(32, '0');
    for (int half = 0; half < 2; ++half) {
        std::uint64_t v = rng();
        for (int i = 0; i < 16; ++i) {
            id[half * 16 + i] = hex[v & 0xF];
            v >>= 4;
        }
    }
    return id;
}

const Id& IdRemap::operator()(const Id& old)
{
    if (old.empty())
        return old;
    Id& mapped = m_map[old];
    if (mapped.empty())
        mapped = newId();
    return mapped;
}

const Id* IdRemap::find(const Id& old) const
{
    auto it = m_map.find(old);
    return it == m_map.end() ? nullptr : &it->second;
}

ScopedIdSequence::ScopedIdSequence(std::string prefix)
    : m_wasActive(t_sequence.active)
    , m_previousPrefix(t_sequence.prefix)
    , m_previousNext(t_sequence.next)
{
    t_sequence = {true, std::move(prefix), 1};
}

ScopedIdSequence::~ScopedIdSequence()
{
    t_sequence = {m_wasActive, m_previousPrefix, m_previousNext};
}

} // namespace ve
