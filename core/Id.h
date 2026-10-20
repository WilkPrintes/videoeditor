#pragma once

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

private:
    std::map<Id, Id> m_map;
};

} // namespace ve
