// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "config/formats.hpp"
#include "mxlbridge/instance.hpp"
#include "util/uuid.hpp"

namespace mxlgw::mxlbridge
{
    /// Test signal of tools/mxl-pattern-writer (§17.3): v210 colour bars with the grain index as frame
    /// counter, a tone locked to the absolute sample index, and SMPTE 12M time code ANC whose user bits
    /// carry the grain index.
    struct PatternConfig
    {
        std::string label = "pattern";
        std::optional<util::Uuid> videoFlow;
        config::VideoFormat video;
        std::optional<util::Uuid> audioFlow;
        config::AudioFormat audio;
        std::optional<util::Uuid> ancFlow;
        config::AncFormat anc;
        double toneHz = 997.0;
        float level = 0.1f;
    };

    class PatternWriter
    {
    public:
        PatternWriter(std::shared_ptr<Instance> instance, PatternConfig config);
        ~PatternWriter();
        PatternWriter(PatternWriter const&) = delete;
        PatternWriter& operator=(PatternWriter const&) = delete;

        void start();
        void stop();
        std::uint64_t grainsWritten() const { return _grains.load(); }
        std::uint64_t samplesWritten() const { return _samples.load(); }
        std::uint64_t errors() const { return _errors.load(); }

    private:
        void run();

        std::shared_ptr<Instance> _instance;
        PatternConfig _config;
        std::atomic<bool> _stop{false};
        std::atomic<std::uint64_t> _grains{0};
        std::atomic<std::uint64_t> _samples{0};
        std::atomic<std::uint64_t> _errors{0};
        std::thread _thread;
    };

    struct VerifyConfig
    {
        std::optional<util::Uuid> videoFlow;
        config::VideoFormat video;
        std::optional<util::Uuid> audioFlow;
        config::AudioFormat audio;
        std::optional<util::Uuid> ancFlow;
        config::AncFormat anc;
        double toneHz = 997.0;
        float level = 0.1f;
        std::chrono::milliseconds duration{5000};
    };

    /// Result of tools/mxl-verify: frame counter continuity, colour bars (proves real v210), tone
    /// frequency/level/phase per channel, time code continuity and A/V alignment.
    struct VerifyReport
    {
        std::uint64_t videoGrains = 0;
        std::uint64_t videoCounterJumps = 0;
        std::uint64_t videoBarsErrors = 0;
        std::uint64_t videoInvalid = 0;
        std::uint64_t readerLate = 0;                  // the verifier itself fell behind the ring (not a pipeline error)
        std::optional<std::int64_t> videoOffsetGrains; // index - counter
        std::uint64_t audioBlocks = 0;
        std::uint64_t audioBadBlocks = 0;
        std::optional<std::int64_t> audioOffsetSamples; // index - source sample index
        double audioMaxError = 0.0;
        std::uint64_t ancGrains = 0;
        std::uint64_t ancCounterJumps = 0;
        std::uint64_t ancMissing = 0;
        std::optional<std::int64_t> ancOffsetGrains;
        std::optional<std::int64_t> avMisalignmentSamples; // audio offset - video offset (in samples)
        std::vector<std::string> failures;

        bool ok() const { return failures.empty(); }
        nlohmann::json toJson() const;
    };

    VerifyReport verifyFlows(std::shared_ptr<Instance> instance, VerifyConfig const& config);
}
