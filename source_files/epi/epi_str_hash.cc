// Copyright (c) 2008-2022 the Urho3D project.
// Copyright (c) 2024-2024 the rbfx project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

#include "epi_str_hash.h"

#include "epi.h"
#ifdef EPI_EXTRA_CHECKS
#include "epi_str_compare.h"
#endif
#include "epi_str_util.h"

namespace epi
{

static const int         CONVERSION_BUFFER_LENGTH = 128;
static const std::string EMPTY_STRING{};

const StringHash StringHash::kEmpty{""};

#ifdef EPI_EXTRA_CHECKS
std::unordered_map<StringHash, std::string> StringHash::global_hash_registry_;
void                                        StringHash::Register(StringHash hash, const std::string_view &str)
{
    std::unordered_map<StringHash, std::string>::iterator iter = global_hash_registry_.find(hash);
    if (iter == global_hash_registry_.end())
    {
        global_hash_registry_.emplace(hash, std::string(str));
    }
    else if (epi::StringCaseCompareASCII(iter->second, str) != 0)
    {
        FatalError("StringHash collision detected! Both \"%s\" and \"%s\" have hash #%s", std::string(str).c_str(),
                   iter->second.c_str(), hash.ToString().c_str());
    }
}
void StringHash::Register(const char *str)
{
    Register(StringHash(str), str);
}
std::string StringHash::GetRegistered(StringHash hash)
{
    std::unordered_map<StringHash, std::string>::iterator iter = global_hash_registry_.find(hash);
    return iter == global_hash_registry_.end() ? EMPTY_STRING : iter->second;
}
const std::unordered_map<StringHash, std::string> &StringHash::GetHashRegistry()
{
    return global_hash_registry_;
}
#endif

std::string StringHash::ToString() const
{
    char tempBuffer[CONVERSION_BUFFER_LENGTH];
    epi::FormatToBufferSized(tempBuffer, sizeof(tempBuffer), "%08X", value_);
    return std::string(tempBuffer);
}

std::string StringHash::ToDebugString() const
{
#ifdef EPI_EXTRA_CHECKS
    return epi::StringFormat("#%s '%s'", ToString().c_str(), Reverse().c_str());
#else
    return epi::StringFormat("#%s", ToString().c_str());
#endif
}

std::string StringHash::Reverse() const
{
#ifdef EPI_EXTRA_CHECKS
    const std::string copy = GetRegistered(*this);
    return copy;
#else
    return EMPTY_STRING;
#endif
}

} // namespace epi
