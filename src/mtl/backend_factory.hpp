// SPDX-License-Identifier: MIT
#pragma once

#include <memory>

#include "config/config.hpp"
#include "mtl/backend.hpp"

namespace mxlgw::media
{
    /// mock (in-process), kernel (MTL kernel-socket PMD, test-only) or dpdk (MTL on E810) — §17.2.
    /// Throws std::runtime_error if the backend is unavailable in this build or fails to start.
    std::unique_ptr<MediaBackend> createBackend(config::Config const& config);
    bool mtlAvailable();
}
