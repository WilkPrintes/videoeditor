#pragma once

#include <cstdint>
#include <map>
#include <string>

namespace ve {

// 128-bit random identifier rendered as 32 lowercase hex characters.
using Id = std::string;

Id newId();

// Gives every old id one fresh replacement for the duration of a single edit, so clips that
// are split or cut on several tracks at once keep a shared link id on the new pieces.
class IdRemap {
public:
    const Id& operator()(const Id& old);
    // The replacement given to `old` so far, or nullptr.
    const Id* find(const Id& old) const;

private:
    std::map<Id, Id> m_map;
};

// Test support: while one is alive, newId() on this thread returns "<prefix>0001", "<prefix>0002"...
// so golden states are reproducible. Scopes nest; the previous sequence comes back on destruction.
class ScopedIdSequence {
public:
    explicit ScopedIdSequence(std::string prefix = "id");
    ~ScopedIdSequence();
    ScopedIdSequence(const ScopedIdSequence&) = delete;
    ScopedIdSequence& operator=(const ScopedIdSequence&) = delete;

private:
    bool m_wasActive;
    std::string m_previousPrefix;
    std::uint64_t m_previousNext;
};

} // namespace ve
