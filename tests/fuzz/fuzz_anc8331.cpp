// SPDX-License-Identifier: MIT
// libFuzzer target: RFC 8331 grain parser (§6.3, §18 robustness). Any input must parse or be
// rejected without crashing; a parsed frame must serialise and re-parse to the same packets.
#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include "codec/anc8331.hpp"

extern "C" int LLVMFuzzerTestOneInput(std::uint8_t const* data, std::size_t size)
{
    using namespace mxlgw::codec;
    auto const parsed = parseGrain(data, size);
    if (!parsed.frame)
    {
        return 0;
    }
    auto const bytes = serialiseGrain(*parsed.frame, 1 << 20);
    if (!bytes)
    {
        return 0;
    }
    auto const again = parseGrain(bytes->data(), bytes->size());
    if (!again.frame || again.frame->packets.size() != parsed.frame->packets.size() || again.frame->field != parsed.frame->field)
    {
        std::abort();
    }
    for (std::size_t i = 0; i < again.frame->packets.size(); ++i)
    {
        if (!(again.frame->packets[i] == parsed.frame->packets[i]))
        {
            std::abort();
        }
    }
    if (again.parityErrors != 0 || again.checksumErrors != 0)
    {
        std::abort();
    }
    return 0;
}
