// SPDX-License-Identifier: MIT
// mxl-pattern-writer: writes v210 colour bars with a frame counter, a tone per channel and SMPTE 12M
// time code ANC into an MXL domain in real time (SPECIFICATION.md §17.3).
#include <csignal>
#include <cstdio>
#include <thread>

#include "../common/args.hpp"
#include "mxlbridge/instance.hpp"
#include "mxlbridge/pattern.hpp"
#include "version.hpp"

namespace
{
    volatile std::sig_atomic_t stopRequested = 0;
    void onSignal(int)
    {
        stopRequested = 1;
    }

    void usage()
    {
        std::puts("usage: mxl-pattern-writer --domain <path> [--video-flow <uuid>] [--audio-flow <uuid>] [--anc-flow <uuid>]\n"
                  "                          [--width 1920] [--height 1080] [--rate 50/1] [--interlace progressive|interlaced_tff|interlaced_bff]\n"
                  "                          [--channels 2] [--block-us 1000] [--audio-delay-us 0] [--tone-hz 997] [--label pattern]\n"
                  "                          [--duration-ms 0]");
    }
}

int main(int argc, char** argv)
{
    using namespace mxlgw;
    tools::Args args(argc, argv);
    if (args.has("help") || !args.has("domain"))
    {
        usage();
        return args.has("help") ? 0 : 2;
    }
    if (args.has("version"))
    {
        std::printf("mxl-pattern-writer %s (MXL %s)\n", std::string(version::gateway).c_str(), mxlbridge::mxlVersionString().c_str());
        return 0;
    }
    mxlbridge::PatternConfig pc;
    pc.label = args.get("label", "pattern");
    pc.videoFlow = args.uuid("video-flow");
    pc.audioFlow = args.uuid("audio-flow");
    pc.ancFlow = args.uuid("anc-flow");
    pc.video = args.video();
    pc.audio = args.audio();
    pc.anc = args.anc();
    pc.toneHz = static_cast<double>(args.integer("tone-hz", 997));
    pc.audioDelayNs = args.integer("audio-delay-us", 0) * 1000;
    if (!pc.videoFlow && !pc.audioFlow && !pc.ancFlow)
    {
        std::fputs("at least one of --video-flow, --audio-flow, --anc-flow is required\n", stderr);
        return 2;
    }
    try
    {
        auto instance = std::make_shared<mxlbridge::Instance>(args.get("domain"));
        mxlbridge::PatternWriter writer(instance, pc);
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);
        writer.start();
        std::printf("{\"event\":\"pattern_writer_started\",\"domain\":\"%s\"}\n", args.get("domain").c_str());
        std::fflush(stdout);
        auto const durationMs = args.integer("duration-ms", 0);
        auto const start = std::chrono::steady_clock::now();
        while (stopRequested == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (durationMs > 0 && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(durationMs))
            {
                break;
            }
        }
        writer.stop();
        std::printf("{\"event\":\"pattern_writer_stopped\",\"grains\":%llu,\"samples\":%llu,\"errors\":%llu}\n",
                    static_cast<unsigned long long>(writer.grainsWritten()), static_cast<unsigned long long>(writer.samplesWritten()),
                    static_cast<unsigned long long>(writer.errors()));
        return writer.errors() == 0 ? 0 : 1;
    }
    catch (std::exception const& ex)
    {
        std::fprintf(stderr, "mxl-pattern-writer: %s\n", ex.what());
        return 1;
    }
}
