// SPDX-License-Identifier: MIT
// mxl-verify: reads flows written by the gateway's ingest from a pattern source and checks frame
// counter continuity, real v210 colour bars, tone frequency/level/phase per channel, time code
// continuity and A/V alignment (SPECIFICATION.md §17.3). Prints a JSON report; exit 0 = pass.
#include <cstdio>

#include "../common/args.hpp"
#include "mxlbridge/instance.hpp"
#include "mxlbridge/pattern.hpp"

int main(int argc, char** argv)
{
    using namespace mxlgw;
    tools::Args args(argc, argv);
    if (args.has("help") || !args.has("domain"))
    {
        std::puts("usage: mxl-verify --domain <path> [--video-flow <uuid>] [--audio-flow <uuid>] [--anc-flow <uuid>]\n"
                  "                  [--width 1920] [--height 1080] [--rate 50/1] [--interlace ...] [--channels 2] [--block-us 1000]\n"
                  "                  [--tone-hz 997] [--duration-ms 5000] [--expect-offset-grains N]");
        return args.has("help") ? 0 : 2;
    }
    mxlbridge::VerifyConfig vc;
    vc.videoFlow = args.uuid("video-flow");
    vc.audioFlow = args.uuid("audio-flow");
    vc.ancFlow = args.uuid("anc-flow");
    vc.video = args.video();
    vc.audio = args.audio();
    vc.anc = args.anc();
    vc.toneHz = static_cast<double>(args.integer("tone-hz", 997));
    vc.duration = std::chrono::milliseconds(args.integer("duration-ms", 5000));
    try
    {
        auto instance = std::make_shared<mxlbridge::Instance>(args.get("domain"));
        auto report = mxlbridge::verifyFlows(instance, vc);
        if (args.has("expect-offset-grains") && report.videoOffsetGrains && *report.videoOffsetGrains != args.integer("expect-offset-grains", 0))
        {
            report.failures.push_back("video offset " + std::to_string(*report.videoOffsetGrains) + " grains, expected " + args.get("expect-offset-grains"));
        }
        std::puts(report.toJson().dump(2).c_str());
        return report.ok() ? 0 : 1;
    }
    catch (std::exception const& ex)
    {
        std::fprintf(stderr, "mxl-verify: %s\n", ex.what());
        return 1;
    }
}
