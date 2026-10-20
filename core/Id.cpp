#include "core/Id.h"

#include <cstdint>
#include <random>

namespace ve {

Id newId()
{
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

} // namespace ve
