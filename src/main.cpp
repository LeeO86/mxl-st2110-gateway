// SPDX-License-Identifier: MIT
// mxl-st2110-gateway — SMPTE ST 2110 <-> MXL gateway (SPECIFICATION.md).
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <thread>

#include <pthread.h>

#include "app/application.hpp"
#include "mxlbridge/instance.hpp"
#include "util/logging.hpp"
#include "version.hpp"

namespace
{
    void usage()
    {
        std::puts("usage: mxl-st2110-gateway [--config <path>] [--preflight] [--version] [--help]\n"
                  "  --config <path>  configuration file (default $MXLGW_CONFIG or /config/gateway.json)\n"
                  "  --preflight      run the preflight checks (§14.3), print them as JSON and exit (78 on failure)\n"
                  "  --version        print the gateway and dependency versions");
    }
}

int main(int argc, char** argv)
{
    using namespace mxlgw;
    app::Options options;
    if (auto const* env = std::getenv("MXLGW_CONFIG"); env != nullptr && *env != '\0')
    {
        options.configPath = env;
    }
    for (int i = 1; i < argc; ++i)
    {
        std::string const arg = argv[i];
        if (arg == "--help" || arg == "-h")
        {
            usage();
            return 0;
        }
        if (arg == "--version")
        {
            std::printf("mxl-st2110-gateway %s\nMTL %s\nDPDK %s\nMXL %s (library %s)\nnmos-cpp %s\n", version::gateway, version::mtl, version::dpdk,
                        version::mxl, mxlbridge::mxlVersionString().c_str(), version::nmosCpp);
            return 0;
        }
        if (arg == "--preflight")
        {
            options.preflightOnly = true;
            continue;
        }
        if (arg == "--config" && i + 1 < argc)
        {
            options.configPath = argv[++i];
            continue;
        }
        std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
        usage();
        return 2;
    }

    // Signals are handled by one thread with sigwait() so the handler code may lock and log (§14.4).
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    app::Application application(options);
    std::thread signalThread(
        [&]
        {
            int sig = 0;
            if (sigwait(&signals, &sig) == 0 && !application.stopping())
            {
                application.stop(app::exitOk);
            }
        });
    auto const rc = application.run();
    if (signalThread.joinable())
    {
        // Wake the signal thread if we stopped for another reason.
        pthread_kill(signalThread.native_handle(), SIGTERM);
        signalThread.join();
    }
    log::stopDrain();
    return rc;
}
