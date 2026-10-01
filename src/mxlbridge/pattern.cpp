// SPDX-License-Identifier: MIT
#include "mxlbridge/pattern.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <mxl/flow.h>

#include "codec/anc8331.hpp"
#include "codec/testpattern.hpp"
#include "codec/v210.hpp"
#include "mxlbridge/flowdef.hpp"
#include "mxlbridge/reader.hpp"
#include "mxlbridge/writer.hpp"
#include "timing/rtpclock.hpp"
#include "util/logging.hpp"
#include "util/threading.hpp"

namespace mxlgw::mxlbridge
{
    namespace
    {
        std::int64_t taiNow()
        {
            timespec ts{};
            ::clock_gettime(CLOCK_TAI, &ts);
            return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000LL + ts.tv_nsec;
        }

        FlowIdentity identity(util::Uuid const& flow, std::string const& label, std::string const& role)
        {
            FlowIdentity id;
            id.flowId = flow;
            id.sourceId = util::uuidV5(flow, "source");
            id.deviceId = util::uuidV5(flow, "device");
            id.label = label + " " + role;
            id.description = "mxl-pattern-writer " + role;
            id.groupHint = label + ":" + role + " 1";
            id.version = nmosVersion(taiNow());
            return id;
        }

        constexpr int counterLines = 16;
    }

    PatternWriter::PatternWriter(std::shared_ptr<Instance> instance, PatternConfig config)
        : _instance(std::move(instance))
        , _config(std::move(config))
    {}

    PatternWriter::~PatternWriter()
    {
        stop();
    }

    void PatternWriter::start()
    {
        if (_thread.joinable())
        {
            return;
        }
        _stop = false;
        _thread = std::thread([this] { run(); });
    }

    void PatternWriter::stop()
    {
        _stop = true;
        if (_thread.joinable())
        {
            _thread.join();
        }
    }

    void PatternWriter::run()
    {
        util::setThreadName("pattern-writer");
        std::unique_ptr<GrainWriter> video;
        std::unique_ptr<SampleWriter> audio;
        std::unique_ptr<GrainWriter> anc;
        try
        {
            if (_config.videoFlow)
            {
                auto const lines = static_cast<std::uint32_t>(_config.video.linesPerGrain());
                video = std::make_unique<GrainWriter>(_instance, videoFlowDef(identity(*_config.videoFlow, _config.label, "Video"), _config.video),
                                                      writerOptions(lines, lines));
            }
            if (_config.audioFlow)
            {
                auto const n = static_cast<std::uint32_t>(_config.audio.samplesPerBlock());
                audio = std::make_unique<SampleWriter>(_instance, audioFlowDef(identity(*_config.audioFlow, _config.label, "Audio"), _config.audio),
                                                       writerOptions(n, n));
            }
            if (_config.ancFlow)
            {
                anc = std::make_unique<GrainWriter>(
                    _instance, ancFlowDef(identity(*_config.ancFlow, _config.label, "Data"), _config.anc),
                    writerOptions(static_cast<std::uint32_t>(config::ancGrainBytes), static_cast<std::uint32_t>(config::ancGrainBytes)));
            }
        }
        catch (std::exception const& ex)
        {
            log::error("pattern_writer_failed", {{"error", ex.what()}});
            _errors.fetch_add(1);
            return;
        }

        auto const stride = _config.video.v210Stride();
        auto const lines = _config.video.linesPerGrain();
        std::vector<std::uint8_t> base(video ? _config.video.grainBytes() : 0);
        if (video)
        {
            codec::v210ColourBars(base.data(), _config.video.width, lines, stride, 0, counterLines);
        }
        auto const videoRate = _config.video.grainRate();
        auto const ancRate = _config.anc.grainRate();
        auto const sr = _config.audio.sampleRate;
        auto const n = static_cast<std::int64_t>(std::max(1, _config.audio.samplesPerBlock()));
        auto const fps = static_cast<int>(std::lround(_config.anc.rate.value()));

        auto const start = taiNow();
        auto nextVideo = timing::timestampToIndex(videoRate, start);
        auto nextAnc = timing::timestampToIndex(ancRate, start);
        auto nextAudioEnd = (timing::ticksAt(start, sr) / n) * n;

        while (!_stop)
        {
            auto const now = taiNow();
            if (video)
            {
                while (timing::indexToTimestamp(videoRate, nextVideo + 1) <= now)
                {
                    mxlGrainInfo info{};
                    std::uint8_t* payload = nullptr;
                    if (video->open(nextVideo, info, payload) == MXL_STATUS_OK)
                    {
                        std::memcpy(payload, base.data(), std::min<std::size_t>(base.size(), info.grainSize));
                        codec::v210ColourBars(payload, _config.video.width, counterLines, stride, static_cast<std::uint32_t>(nextVideo), counterLines);
                        info.flags = 0;
                        info.validSlices = info.totalSlices;
                        if (video->commit(info) == MXL_STATUS_OK)
                        {
                            _grains.fetch_add(1);
                        }
                        else
                        {
                            _errors.fetch_add(1);
                        }
                    }
                    else
                    {
                        _errors.fetch_add(1);
                    }
                    ++nextVideo;
                }
            }
            if (anc)
            {
                while (timing::indexToTimestamp(ancRate, nextAnc + 1) <= now)
                {
                    codec::AncFrame frame;
                    if (_config.anc.interlace != config::Interlace::Progressive)
                    {
                        frame.field = nextAnc % 2 == 0 ? codec::AncField::Field1 : codec::AncField::Field2;
                    }
                    frame.packets.push_back(codec::timecodePacket(static_cast<std::uint32_t>(nextAnc), fps));
                    auto const body = codec::serialiseGrain(frame, config::ancGrainBytes);
                    if (body && anc->write(nextAnc, *body) == MXL_STATUS_OK)
                    {
                        _grains.fetch_add(1);
                    }
                    else
                    {
                        _errors.fetch_add(1);
                    }
                    ++nextAnc;
                }
            }
            if (audio)
            {
                while (timing::taiOfTicks(nextAudioEnd + n, sr) <= now)
                {
                    auto const end = nextAudioEnd + n;
                    mxlMutableWrappedMultiBufferSlice slices{};
                    if (audio->open(static_cast<std::uint64_t>(end), static_cast<std::size_t>(n), slices) == MXL_STATUS_OK)
                    {
                        auto const first = end - n;
                        for (std::size_t c = 0; c < slices.count; ++c)
                        {
                            std::int64_t s = first;
                            for (auto const& frag : slices.base.fragments)
                            {
                                auto* p = reinterpret_cast<float*>(static_cast<std::uint8_t*>(frag.pointer) + c * slices.stride);
                                for (std::size_t j = 0; j < frag.size / sizeof(float); ++j)
                                {
                                    p[j] = codec::toneSample(s++, static_cast<int>(c), _config.toneHz, _config.level, sr);
                                }
                            }
                        }
                        if (audio->commit() == MXL_STATUS_OK)
                        {
                            _samples.fetch_add(static_cast<std::uint64_t>(n));
                        }
                    }
                    else
                    {
                        _errors.fetch_add(1);
                    }
                    nextAudioEnd += n;
                }
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    nlohmann::json VerifyReport::toJson() const
    {
        nlohmann::json j;
        auto opt = [](std::optional<std::int64_t> const& v) { return v ? nlohmann::json(*v) : nlohmann::json(); };
        j["ok"] = ok();
        j["video"] = {{"grains", videoGrains},   {"counter_jumps", videoCounterJumps}, {"bars_errors", videoBarsErrors},
                      {"invalid", videoInvalid}, {"reader_late", readerLate},          {"offset_grains", opt(videoOffsetGrains)}};
        j["audio"] = {{"blocks", audioBlocks}, {"bad_blocks", audioBadBlocks}, {"max_error", audioMaxError}, {"offset_samples", opt(audioOffsetSamples)}};
        j["anc"] = {{"grains", ancGrains}, {"counter_jumps", ancCounterJumps}, {"missing", ancMissing}, {"offset_grains", opt(ancOffsetGrains)}};
        j["av_misalignment_samples"] = opt(avMisalignmentSamples);
        j["failures"] = failures;
        return j;
    }

    namespace
    {
        template <typename Reader>
        std::unique_ptr<Reader> openWithRetry(std::shared_ptr<Instance> const& instance, util::Uuid const& flow, std::int64_t until)
        {
            while (taiNow() < until)
            {
                mxlStatus status = MXL_ERR_UNKNOWN;
                if (auto r = Reader::open(instance, flow.toString(), status))
                {
                    return r;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            return nullptr;
        }

        std::int64_t wrapDiff(std::uint64_t index, std::uint32_t counter)
        {
            return static_cast<std::int32_t>(static_cast<std::uint32_t>(index) - counter);
        }

        double blockError(mxlWrappedMultiBufferSlice const& slices, std::int64_t firstSample, std::int64_t offset, VerifyConfig const& c)
        {
            double maxErr = 0.0;
            for (std::size_t ch = 0; ch < slices.count; ++ch)
            {
                std::int64_t s = firstSample;
                for (auto const& frag : slices.base.fragments)
                {
                    auto const* p = reinterpret_cast<float const*>(static_cast<std::uint8_t const*>(frag.pointer) + ch * slices.stride);
                    for (std::size_t j = 0; j < frag.size / sizeof(float); ++j)
                    {
                        auto const expected = codec::toneSample(s - offset, static_cast<int>(ch), c.toneHz, c.level, c.audio.sampleRate);
                        maxErr = std::max(maxErr, std::fabs(static_cast<double>(p[j]) - static_cast<double>(expected)));
                        ++s;
                    }
                }
            }
            return maxErr;
        }
    }

    VerifyReport verifyFlows(std::shared_ptr<Instance> instance, VerifyConfig const& c)
    {
        VerifyReport report;
        constexpr std::int64_t unknownOffset = INT64_MIN;
        std::atomic<std::int64_t> videoOffset{unknownOffset};
        auto const until = taiNow() + std::chrono::duration_cast<std::chrono::nanoseconds>(c.duration).count();

        std::thread videoThread;
        if (c.videoFlow)
        {
            videoThread = std::thread(
                [&]
                {
                    auto reader = openWithRetry<GrainReader>(instance, *c.videoFlow, until);
                    if (!reader)
                    {
                        return;
                    }
                    auto const stride = c.video.v210Stride();
                    auto idx = reader->runtime() ? reader->runtime()->headIndex + 1 : 0;
                    while (taiNow() < until)
                    {
                        mxlGrainInfo info{};
                        std::uint8_t const* payload = nullptr;
                        auto const status = reader->get(idx, 200'000'000, info, payload);
                        if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY)
                        {
                            continue;
                        }
                        if (status != MXL_STATUS_OK)
                        {
                            // Fell out of the ring: resync. Lost frames would show as invalid grains or an offset change.
                            ++report.readerLate;
                            idx = reader->runtime() ? reader->runtime()->headIndex : idx + 1;
                            continue;
                        }
                        ++report.videoGrains;
                        if ((info.flags & MXL_GRAIN_FLAG_INVALID) != 0)
                        {
                            ++report.videoInvalid;
                        }
                        else if (auto const counter = codec::v210ReadCounter(payload, c.video.width, stride, counterLines))
                        {
                            auto const off = wrapDiff(idx, *counter);
                            if (!report.videoOffsetGrains)
                            {
                                report.videoOffsetGrains = off;
                                videoOffset.store(off);
                            }
                            else if (*report.videoOffsetGrains != off)
                            {
                                ++report.videoCounterJumps;
                                report.videoOffsetGrains = off;
                            }
                            if (report.videoGrains % 10 == 1 && !codec::v210CheckBars(payload, c.video.width, c.video.linesPerGrain(), stride, counterLines))
                            {
                                ++report.videoBarsErrors;
                            }
                        }
                        else
                        {
                            ++report.videoBarsErrors; // no readable counter: not our v210 pattern
                        }
                        ++idx;
                    }
                });
        }

        std::thread ancThread;
        if (c.ancFlow)
        {
            ancThread = std::thread(
                [&]
                {
                    auto reader = openWithRetry<GrainReader>(instance, *c.ancFlow, until);
                    if (!reader)
                    {
                        return;
                    }
                    auto idx = reader->runtime() ? reader->runtime()->headIndex + 1 : 0;
                    while (taiNow() < until)
                    {
                        mxlGrainInfo info{};
                        std::uint8_t const* payload = nullptr;
                        auto const status = reader->get(idx, 200'000'000, info, payload);
                        if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY)
                        {
                            continue;
                        }
                        if (status != MXL_STATUS_OK)
                        {
                            ++report.readerLate;
                            idx = reader->runtime() ? reader->runtime()->headIndex : idx + 1;
                            continue;
                        }
                        ++report.ancGrains;
                        auto const parsed = codec::parseGrain(payload, std::min<std::size_t>(info.grainSize, config::ancGrainBytes));
                        auto const counter = parsed.frame ? codec::timecodeCounter(*parsed.frame) : std::nullopt;
                        if (!counter)
                        {
                            ++report.ancMissing;
                        }
                        else
                        {
                            auto const off = wrapDiff(idx, *counter);
                            if (!report.ancOffsetGrains)
                            {
                                report.ancOffsetGrains = off;
                            }
                            else if (*report.ancOffsetGrains != off)
                            {
                                ++report.ancCounterJumps;
                                report.ancOffsetGrains = off;
                            }
                        }
                        ++idx;
                    }
                });
        }

        std::thread audioThread;
        if (c.audioFlow)
        {
            audioThread = std::thread(
                [&]
                {
                    auto reader = openWithRetry<SampleReader>(instance, *c.audioFlow, until);
                    if (!reader)
                    {
                        return;
                    }
                    auto const n = static_cast<std::int64_t>(std::max(1, c.audio.samplesPerBlock()));
                    auto end = reader->runtime() ? static_cast<std::int64_t>(reader->runtime()->headIndex) : 0;
                    end = (end / n) * n;
                    while (taiNow() < until)
                    {
                        end += n;
                        mxlWrappedMultiBufferSlice slices{};
                        auto status = reader->get(static_cast<std::uint64_t>(end), static_cast<std::size_t>(n), 200'000'000, slices);
                        if (status == MXL_ERR_OUT_OF_RANGE_TOO_EARLY)
                        {
                            end -= n;
                            continue;
                        }
                        if (status == MXL_ERR_OUT_OF_RANGE_TOO_LATE)
                        {
                            ++report.readerLate;
                            end = reader->runtime() ? (static_cast<std::int64_t>(reader->runtime()->headIndex) / n - 1) * n : end;
                            continue;
                        }
                        if (status != MXL_STATUS_OK)
                        {
                            ++report.audioBadBlocks;
                            continue;
                        }
                        ++report.audioBlocks;
                        auto const first = end - n;
                        if (!report.audioOffsetSamples)
                        {
                            // Seed the search with the video offset (A/V alignment is what we check), then search the
                            // whole +-0.5 s window for the best match.
                            auto const perGrain =
                                static_cast<std::int64_t>(std::llround(static_cast<double>(c.audio.sampleRate) / c.video.grainRate().value()));
                            for (int wait = 0; wait < 50 && c.videoFlow && videoOffset.load() == unknownOffset; ++wait)
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                            }
                            std::int64_t best = 0;
                            double bestErr = 1e9;
                            if (auto const hint = videoOffset.load(); hint != unknownOffset)
                            {
                                best = hint * perGrain;
                                bestErr = blockError(slices, first, best, c);
                            }
                            for (std::int64_t i = 0; i <= c.audio.sampleRate && bestErr > 2e-5; ++i)
                            {
                                auto const d = i % 2 == 0 ? i / 2 : -(i + 1) / 2;
                                auto const err = blockError(slices, first, d, c);
                                if (err < bestErr)
                                {
                                    bestErr = err;
                                    best = d;
                                }
                            }
                            report.audioOffsetSamples = best;
                        }
                        auto const err = blockError(slices, first, *report.audioOffsetSamples, c);
                        report.audioMaxError = std::max(report.audioMaxError, err);
                        if (err > 1e-3)
                        {
                            ++report.audioBadBlocks;
                        }
                    }
                });
        }

        for (auto* t : {&videoThread, &ancThread, &audioThread})
        {
            if (t->joinable())
            {
                t->join();
            }
        }

        if (c.videoFlow)
        {
            if (report.videoGrains == 0)
            {
                report.failures.emplace_back("no video grains");
            }
            if (report.videoCounterJumps > 0)
            {
                report.failures.emplace_back("video frame counter not continuous");
            }
            if (report.videoBarsErrors > 0)
            {
                report.failures.emplace_back("video pattern is not valid v210 colour bars");
            }
            if (report.videoInvalid > 0)
            {
                report.failures.emplace_back("video grains missing (marked invalid)");
            }
        }
        if (c.audioFlow)
        {
            if (report.audioBlocks == 0)
            {
                report.failures.emplace_back("no audio samples");
            }
            if (report.audioBadBlocks > 0)
            {
                report.failures.emplace_back("audio tone frequency/level/phase mismatch");
            }
        }
        if (c.ancFlow)
        {
            if (report.ancGrains == 0)
            {
                report.failures.emplace_back("no ANC grains");
            }
            if (report.ancCounterJumps > 0 || report.ancMissing > 0)
            {
                report.failures.emplace_back("time code not continuous");
            }
        }
        if (report.videoOffsetGrains && report.audioOffsetSamples)
        {
            auto const perGrain = static_cast<std::int64_t>(std::llround(static_cast<double>(c.audio.sampleRate) / c.video.grainRate().value()));
            report.avMisalignmentSamples = *report.audioOffsetSamples - *report.videoOffsetGrains * perGrain;
            if (std::llabs(*report.avMisalignmentSamples) > c.audio.samplesPerBlock())
            {
                report.failures.emplace_back("audio/video misaligned by more than one audio block");
            }
        }
        if (report.videoOffsetGrains && report.ancOffsetGrains && *report.videoOffsetGrains != *report.ancOffsetGrains)
        {
            report.failures.emplace_back("ANC and video misaligned");
        }
        return report;
    }
}
