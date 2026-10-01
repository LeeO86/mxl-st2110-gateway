// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>

#include "codec/anc8331.hpp"

namespace mxlgw::codec
{
    /// Test tone used by tools/mxl-pattern-writer and mxl-verify (§17.3): a sine locked to the
    /// absolute sample index, so the phase of every sample is known and A/V alignment can be checked.
    /// Channel c uses the non-harmonic frequency `hz * (1 + 0.5 k) + 7 k` with k = c % 4, so channel order
    /// and alignment are verifiable.
    float toneSample(std::int64_t sampleIndex, int channel, double hz, float level, int sampleRate);

    /// SMPTE 12M-2 ancillary time code packet (DID 0x60, SDID 0x60, 16 UDW, RP 188 layout): BCD time
    /// code in the 8 time-address UDWs and a 32-bit frame counter in the 8 binary-group UDWs.
    AncPacket timecodePacket(std::uint32_t counter, int framesPerSecond, std::uint16_t line = 9);
    /// Decodes the 32-bit counter from a time code packet; nullopt if the packet is not one.
    std::optional<std::uint32_t> timecodeCounter(AncPacket const& packet);
    /// First time code counter in a frame.
    std::optional<std::uint32_t> timecodeCounter(AncFrame const& frame);
}
